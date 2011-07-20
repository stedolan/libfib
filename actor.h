#include "llq.h"
#include "switch.h"
#include "sched.h"
#include "sync_object.h"
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

template <class Self>
class Actor{
  enum MailboxState { MAILBOX_RECVWAIT, MAILBOX_MSGQUEUE };
  fiber_sync_object mailbox;
  llq::queue requeued_mailbox;

public:
  typedef TargetedMessage<Self> AnyMessage;

protected:
  Actor() : mailbox(MAILBOX_MSGQUEUE) {
    llq::queue_init(&requeued_mailbox, 0);
  }

  __attribute__((always_inline)) AnyMessage* receive(){
    using llq::word;
    word state = single_thread_ops::get_state(&requeued_mailbox);
    if (!single_thread_ops::isempty(&requeued_mailbox, state)){
      say("got requeued message\n");
      llq::node* node;
      bool success = single_thread_ops::try_dequeue(&requeued_mailbox, &node, state, 0);
      assert(success);
      return static_cast< AnyMessage* >(node);
    }else{
      dispatch_method meth = mailbox.begin_operation();
      AnyMessage* msg;
      if (meth == D_SINGLETHREAD) msg = receive_impl<single_thread_ops>();
      else                        msg = receive_impl<concurrent_ops>();
      mailbox.end_operation();
      return msg;
    }
  }
  template <class ops>
  __attribute__((always_inline)) AnyMessage* receive_impl(){
    using llq::word;
    while (1){
      llq::queue* q = &mailbox.queue;
      word state = ops::get_state(q);
      assert(ops::tag(q, state) == MAILBOX_MSGQUEUE);
      if (ops::isempty(q, state)){
        waiter<AnyMessage*> self;
        if (ops::try_enqueue(q, &self, state, MAILBOX_RECVWAIT)){
          say("waiting for message\n");
          return worker::current().sleep(self);
        }
      }else{
        llq::node* node;
        if (ops::try_dequeue(q, &node, state, MAILBOX_MSGQUEUE)){
          say("getting queued message\n");
          return static_cast<AnyMessage*>(node);
        }
      }
    }
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
  __attribute__((always_inline)) void send(AnyMessage* mesg){
    dispatch_method meth = mailbox.begin_operation();
    if (meth == D_SINGLETHREAD) send_impl<single_thread_ops>(mesg);
    else                        send_impl<concurrent_ops>(mesg);
    mailbox.end_operation();
  }
  template <class ops>
  __attribute__((always_inline)) void send_impl(AnyMessage* mesg){
    using llq::word;
    while (1){
      llq::queue* q = &mailbox.queue;
      word state = ops::get_state(q);
      if (ops::tag(q, state) == MAILBOX_RECVWAIT){
        assert(!ops::isempty(q, state));
        llq::node* actor_;
        waiter<void> self;
        if (ops::try_dequeue(q, &actor_, state, MAILBOX_MSGQUEUE)){
          say("waking actor\n");
          // Actor is waiting for a message, wake it up
          waiter<AnyMessage*>* actor = static_cast< waiter<AnyMessage*>* >(actor_); 
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

  //template <class Result, int Type>
  //Result request(RequestMessage<Self, Result, Type>* mesg);

  //template <class Result, class OldTarget, int OldType, int NewType>
  //void proxy_request(RequestMessage<OldTarget, Result, OldType>* origmsg, 
  //RequestMessage<Self, Result, NewType>* newmsg);
};
