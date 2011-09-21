#include "llq.h"
#include "switch.h"
#include "sched.h"
#include "spawn.h"
#include "sync_object.h"



// A queue with asynchronous send and blocking receive
template <class T>
struct actor_queue{
  fiber_sync_object sync;
  enum MailboxState { MAILBOX_RECVWAIT, MAILBOX_MSGQUEUE };

  actor_queue() : sync(MAILBOX_MSGQUEUE) {}

  T receive(){
    dispatch_method meth = sync.begin_operation();
    llq::node* msg;
    if (meth == D_SINGLETHREAD) msg = receive_impl<single_thread_ops>();
    else                        msg = receive_impl<concurrent_ops>();
    sync.end_operation();
    return static_cast<T>(msg);
  }

  template <class ops>
  llq::node* receive_impl(){
    using llq::word;
    while (1){
      llq::queue* q = &sync.queue;
      word state = ops::get_state(q);
      assert(ops::tag(q, state) == MAILBOX_MSGQUEUE);
      if (ops::isempty(q, state)){
        waiter<llq::node*> self;
        if (ops::try_enqueue(q, &self, state, MAILBOX_RECVWAIT)){
          say("waiting for message\n");
          return worker::current().sleep(self);
        }
      }else{
        llq::node* node;
        if (ops::try_dequeue(q, &node, state, MAILBOX_MSGQUEUE)){
          say("getting queued message\n");
          return node;
        }
      }
    }
  }

  void send(T mesg){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) send_impl<single_thread_ops>(static_cast<llq::node*>(mesg));
    else                        send_impl<concurrent_ops>(static_cast<llq::node*>(mesg));
    sync.end_operation();
  }

  template <class ops>
  void send_impl(llq::node* mesg){
    using llq::word;
    while (1){
      llq::queue* q = &sync.queue;
      word state = ops::get_state(q);
      if (ops::tag(q, state) == MAILBOX_RECVWAIT){
        assert(!ops::isempty(q, state));
        llq::node* actor_;
        waiter<void> self;
        if (ops::try_dequeue(q, &actor_, state, MAILBOX_MSGQUEUE)){
          say("waking actor\n");
          // Actor is waiting for a message, wake it up
          waiter<llq::node*>* actor = static_cast< waiter<llq::node*>* >(actor_); 
          worker::current().push_runqueue(&self);
          self.invoke(actor, mesg);
          return;
        }
      }else{
        if (ops::try_enqueue(q, mesg, state, MAILBOX_MSGQUEUE)){
          say("enqueuing message\n");
          // Enqueued the message, it will be handled eventually
          return;
        }
      }
    }
  }

  template <class R>
  R req(T mesg, waiter<R>* w){
    dispatch_method meth = sync.begin_operation();
    if (meth == D_SINGLETHREAD) return req_impl<single_thread_ops>(static_cast<llq::node*>(mesg),w);
    else                        return req_impl<concurrent_ops>(static_cast<llq::node*>(mesg),w);
    sync.end_operation();
  }

  template <class ops, class R>
  R req_impl(llq::node* mesg, waiter<R>* w){
    using llq::word;
    while (1){
      llq::queue* q = &sync.queue;
      word state = ops::get_state(q);
      if (ops::tag(q, state) == MAILBOX_RECVWAIT){
        assert(!ops::isempty(q, state));
        llq::node* actor_;
        if (ops::try_dequeue(q, &actor_, state, MAILBOX_MSGQUEUE)){
          say("waking actor\n");
          // Actor is waiting for a message, wake it up
          waiter<llq::node*>* actor = static_cast< waiter<llq::node*>* >(actor_); 
          return w->invoke(actor, mesg);
        }
      }else{
        if (ops::try_enqueue(q, mesg, state, MAILBOX_MSGQUEUE)){
          say("enqueuing message\n");
          // Enqueued the message, it will be handled eventually
          waiter<void>* other = worker::current().pop_runqueue();
          return w->invoke(other);
        }
      }
    }
  }

};





// FIXME: public llq::node??? may this be a use for private inheritance?
class MessageBase : public llq::node{
protected:
  int type;
  MessageBase(int type_) : type(type_) {}
};


template <class TargetActor>
class TargetedMessage : public MessageBase{
protected:
  TargetedMessage(int type_) : MessageBase(type_) {}
public:
  operator typename TargetActor::MessageType () {
    return static_cast<typename TargetActor::MessageType>(type);
  }
  template <class T>
  T& as(){
    assert(T::MessageType == type);
    return static_cast<T&>(*this);
  }
};

template <class TargetActor, int Type, class Payload>
class Message : public TargetedMessage<TargetActor>{
public:
  enum { MessageType = Type };

  Message() : TargetedMessage<TargetActor>(Type) {}
  Message(const Payload& p) 
    : TargetedMessage<TargetActor>(Type), payload(p) {}

  Payload payload;
};

template <class T>
class AsyncReply{
  //FIXME: actor_queue<T> may not be best
  //FIXME: oh god this is slow
  actor_queue< Message<AsyncReply<T>, 0, T>* > chan;
public:
  enum MessageType {A};
  void provide(T data){
    chan.send(new Message<AsyncReply<T>, 0, T>(data));
  }

  T await(){
    Message<AsyncReply<T>, 0, T>* msg = chan.receive();
    T payload = msg->payload;
    delete msg;
    return payload;
  }
};

template <class TargetActor, int Type, class Result, class Payload>
class RequestMessage : public TargetedMessage<TargetActor>{
  enum { WAITER = 0, ASYNC_REPLY = 1 };
  llq::word requestor;


public:
  enum { MessageType = Type };
  RequestMessage() : TargetedMessage<TargetActor>(Type), requestor(0) {}
  RequestMessage(const Payload& p)
    : TargetedMessage<TargetActor>(Type), payload(p), requestor(0) {}

  Payload payload;


  void setup(waiter<Result>* wait){
    requestor = llq::wrap_tagptr(wait, WAITER);
  }
  void setup(AsyncReply<Result>* async){
    requestor = llq::wrap_tagptr(async, ASYNC_REPLY);
  }

  void reply(Result r){
    if (llq::tagptr_tag(requestor) == WAITER){
      waiter<Result>* w = llq::unwrap_tagptr< waiter<Result> >(requestor);
      waiter<void> self;
      worker::current().push_runqueue(&self);
      self.invoke(w, r);
    }else{
      AsyncReply<Result>* a = llq::unwrap_tagptr< AsyncReply<Result> >(requestor);
      a->provide(r);
    }
  }
};
/*
template <class Result>
class RequestMessageBase : public MessageBase{
  waiter<Result>* requestor;
protected:
  RequestMessageBase(waiter<Result>* req_, int type_)
    : MessageBase(type_), requestor(req_) {}
};

template <class TargetActor, class Result, int Type>
class RequestMessage : public RequestMessageBase<Result>{
protected:
  RequestMessage(waiter<Result>* req_)
    : RequestMessageBase<Result>(req_, Type) {}
};
*/

template <class ActorT>
int actor_startup_thunk(ActorT* act){
  act->run();
  return 0;
}

template <class Self>
class Actor{
public:
  typedef TargetedMessage<Self> AnyMessage;

private:
  actor_queue<AnyMessage*> mailbox;
  llq::queue requeued_mailbox;



protected:
  Actor() {
    llq::queue_init(&requeued_mailbox, 0);
  }

  AnyMessage* receive(){
    // FIXME requeued messages
    return mailbox.receive();
  }


  //template <class MesgType>
  //MesgType* receive_only();
  
  void requeue(AnyMessage* mesg){
    word state = single_thread_ops::get_state(&requeued_mailbox);
    bool success = single_thread_ops::try_enqueue(&requeued_mailbox, mesg, state, 0);
    assert(success);
  }

  //template <class Result>
  //void respond(RequestMessage<Result>* mesg, Result arg);

public:

  void send(AnyMessage* msg){
    mailbox.send(msg);
  }

  template <class Result, int Type, class Payload>
  Result request(RequestMessage<Self, Type, Result, Payload>* mesg){
    waiter<Result> w;
    mesg->setup(&w);
    return mailbox.req(mesg, &w);
  }

  template <class Result, int Type, class Payload>
  void request(RequestMessage<Self, Type, Result, Payload>* mesg, AsyncReply<Result>* async){
    mesg->setup(async);
    mailbox.send(mesg);
  }

  void start(int stk = 102400){
    spawn_fiber_fixed<int, Self*, actor_startup_thunk<Self> >(static_cast<Self*>(this), stk);
  }

  //template <class Result, class OldTarget, int OldType, int NewType>
  //void proxy_request(RequestMessage<OldTarget, Result, OldType>* origmsg, 
  //RequestMessage<Self, Result, NewType>* newmsg);
};
