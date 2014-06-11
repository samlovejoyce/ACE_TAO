// -*- C++ -*-
// $Id$

#include "AsyncAccessManager.h"
#include "ImR_Locator_i.h"
#include "Locator_Repository.h"
#include "UpdateableServerInfo.h"
#include "orbsvcs/Log_Macros.h"

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

AsyncAccessManager::AsyncAccessManager (UpdateableServerInfo &info,
                                        bool manual,
                                        ImR_Locator_i &locator)
  :info_(info),
   manual_start_ (manual),
   locator_(locator),
   poa_(locator.root_poa()),
   rh_list_(),
   status_(AAM_INIT),
   refcount_(1),
   lock_()
{
  if (ImR_Locator_i::debug () > 4)
    {
      ORBSVCS_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%P|%t) AsyncAccessManager::ctor server = %s\n"),
                      this, this->info_->ping_id ()));
    }
}

AsyncAccessManager::~AsyncAccessManager (void)
{
}

void
AsyncAccessManager::started_running (void)
{
  this->status_ = AAM_SERVER_STARTED_RUNNING;
}

bool
AsyncAccessManager::has_server (const char *s)
{
  return ACE_OS::strcmp (this->info_->ping_id(), s) == 0;
}

void
AsyncAccessManager::add_interest (ImR_ResponseHandler *rh)
{
  {
    ACE_GUARD (TAO_SYNCH_MUTEX, mon, this->lock_);
    this->rh_list_.push_back (rh);
  }
  if (ImR_Locator_i::debug () > 4)
    {
      ORBSVCS_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%P|%t) AsyncAccessManager::add_interest status = %d\n"),
                                this->status_));
    }


  if (this->info_->is_mode (ImplementationRepository::PER_CLIENT))
    {
      if (!this->send_start_request())
        {
          this->final_state();
        }
      return;
   }

  if (this->status_ == AAM_SERVER_READY || this->status_ == AAM_SERVER_STARTED_RUNNING)
    {
      if (this->locator_.pinger().is_alive (this->info_->ping_id()) == LS_ALIVE)
        {
          this->status_ = AAM_SERVER_READY;
          this->final_state();
          return;
        }
    }

  if (this->status_ == AAM_INIT ||
      this->status_ == AAM_SERVER_READY ||
      this->status_ == AAM_SERVER_STARTED_RUNNING)
    {
      // This is not a leak. The listener registers with
      // the pinger and will delete itself when done.
      AccessLiveListener *l = 0;
      ACE_NEW (l, AccessLiveListener (this->info_->ping_id(),
                                     this,
                                     this->locator_.pinger()));
      LiveListener_ptr llp(l);
      if (!l->start())
        {
          if (!this->send_start_request())
            {
              this->final_state();
            }
        }
      else
        {
          if (this->status_ == AAM_SERVER_STARTED_RUNNING)
            {
              this->status (AAM_WAIT_FOR_ALIVE);
            }
          else
            {
              this->status (AAM_WAIT_FOR_PING);
            }
        }
    }
}

void
AsyncAccessManager::final_state (void)
{
  for (size_t i = 0; i < this->rh_list_.size(); i++)
    {
      ImR_ResponseHandler *rh = this->rh_list_[i];
      if (rh != 0)
        {
          if (this->status_ == AAM_SERVER_READY)
            {
              rh->send_ior (this->info_->partial_ior.c_str());
            }
          else
            {
              try
                {
                  switch (this->status_)
                    {
                    case AAM_NO_ACTIVATOR:
                      throw ImplementationRepository::CannotActivate
                        ("No activator registered for server.");
                    case AAM_NOT_MANUAL:
                      throw ImplementationRepository::CannotActivate
                        ("Cannot implicitly activate MANUAL server.");
                    case AAM_NO_COMMANDLINE:
                      throw ImplementationRepository::CannotActivate
                        ("No command line registered for server.");
                    default:
                      throw ImplementationRepository::CannotActivate
                        ("Unknown Failure");
                    }
                }
              catch (CORBA::Exception &ex)
                {
                  rh->send_exception (ex._tao_duplicate());
                }
            }
        }
    }
  this->rh_list_.clear ();
  if (this->info_->is_mode (ImplementationRepository::PER_CLIENT) ||
      this->status_ != AAM_SERVER_READY)
    {
      AsyncAccessManager_ptr aam (this);
      this->locator_.remove_aam (aam);
      aam._retn(); // release w/o decrementing since table held last reference.
    }
}

AAM_Status
AsyncAccessManager::status (void) const
{
  return this->status_;
}

void
AsyncAccessManager::status (AAM_Status s)
{
  ACE_GUARD (TAO_SYNCH_MUTEX, mon, this->lock_);
  this->status_ = s;
}

void
AsyncAccessManager::activator_replied (bool success)
{
  if (success)
    {
      this->status (AAM_WAIT_FOR_RUNNING);
    }
  else
    {
      this->status (AAM_NO_ACTIVATOR);
      this->final_state ();
    }
}

void
AsyncAccessManager::server_is_shutting_down (void)
{
  this->status (AAM_SERVER_DEAD);
  this->final_state ();
}

void
AsyncAccessManager::server_is_running (const char *partial_ior,
                                       ImplementationRepository::ServerObject_ptr ref)
{
  if (ImR_Locator_i::debug () > 4)
    {
      ORBSVCS_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%P|%t) AsyncAccessManager::server_is_running\n")));
    }

  this->status (AAM_WAIT_FOR_ALIVE);
  this->info_.edit ()->partial_ior = partial_ior;
  this->info_.edit ()->server =
    ImplementationRepository::ServerObject::_duplicate (ref);

  if (this->locator_.pinger().is_alive (this->info_->ping_id()) == LS_ALIVE)
    {
      this->status (AAM_SERVER_READY);
      this->final_state ();
    }

  AccessLiveListener *l = 0;
  if (this->info_->is_mode (ImplementationRepository::PER_CLIENT))
    {
      ACE_NEW (l, AccessLiveListener (this->info_->ping_id(),
                                      this,
                                      this->locator_.pinger(),
                                      this->info_->active_info()->server.in()));
    }
  else
    {
      ACE_NEW (l, AccessLiveListener (this->info_->ping_id(),
                                      this,
                                      this->locator_.pinger()));
    }

  LiveListener_ptr llp(l);
  if (!l->start())
    {
      this->status (AAM_SERVER_DEAD);
      this->final_state ();
    }
}

void
AsyncAccessManager::notify_child_death (void)
{
  if (ImR_Locator_i::debug () > 2)
    {
      ORBSVCS_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%P|%t) AsyncAccessManager, child death\n")));
    }
  this->status (AAM_SERVER_DEAD);
  this->final_state ();
}


void
AsyncAccessManager::ping_replied (LiveStatus server)
{
  if (ImR_Locator_i::debug () > 4)
    {
      ORBSVCS_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%P|%t) AsyncAccessManager::ping_replied %s\n"),
                      LiveEntry::status_name (server)));
    }

  switch (server)
    {
    case LS_ALIVE:
    case LS_LAST_TRANSIENT:
    case LS_TIMEDOUT:
      this->status (AAM_SERVER_READY);
      break;
    case LS_DEAD:
      {
        if (this->status_ == AAM_WAIT_FOR_PING)
          {
            if (this->send_start_request ())
              {
                return;
              }
          }
        else
          {
            this->status (AAM_SERVER_DEAD);
          }
      }
      break;
    default:
      return;
    }
  this->final_state();
}

bool
AsyncAccessManager::send_start_request (void)
{
  if (ImR_Locator_i::debug () > 4)
    {
      ORBSVCS_DEBUG ((LM_DEBUG,
                      ACE_TEXT ("(%P|%t) AsyncAccessManager::send_start_request\n")));
    }

  if (this->info_->is_mode (ImplementationRepository::MANUAL) &&
      !this->manual_start_)
    {
      this->status (AAM_NOT_MANUAL);
      return false;
    }

  const Server_Info *startup = this->info_->active_info ();

  if (startup->cmdline.length () == 0)
    {
      this->status (AAM_NO_COMMANDLINE);
      return false;
    }

  Activator_Info_Ptr ainfo =
    this->locator_.get_activator (startup->activator);

  if (ainfo.null () || CORBA::is_nil (ainfo->activator.in ()))
    {
      this->status (AAM_NO_ACTIVATOR);
      return false;
    }

  PortableServer::ServantBase_var callback = new ActivatorReceiver (this,
                                                                    this->poa_.in());
  PortableServer::ObjectId_var oid = this->poa_->activate_object (callback.in());
  CORBA::Object_var obj = this->poa_->id_to_reference (oid.in());
  ImplementationRepository::AMI_ActivatorHandler_var cb =
    ImplementationRepository::AMI_ActivatorHandler::_narrow (obj.in());

  ainfo->activator->sendc_start_server (cb.in(),
                                        startup->key_name_.c_str (),
                                        startup->cmdline.c_str (),
                                        startup->dir.c_str (),
                                        startup->env_vars);
  this->status (AAM_ACTIVATION_SENT);
  return true;
}

AsyncAccessManager *
AsyncAccessManager::_add_ref (void)
{
  ACE_GUARD_RETURN (TAO_SYNCH_MUTEX, mon, this->lock_, 0);
  ++this->refcount_;

  return this;
}

void
AsyncAccessManager::_remove_ref (void)
{
  int count = 0;
  {
    ACE_GUARD (TAO_SYNCH_MUTEX, mon, this->lock_);
    count = --this->refcount_;
  }
  if (count == 0)
    {
      delete this;
    }
}

//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

ActivatorReceiver::ActivatorReceiver (AsyncAccessManager *aam,
                                      PortableServer::POA_ptr poa)
  :aam_ (aam->_add_ref ()),
   poa_ (PortableServer::POA::_duplicate (poa))
{
}


ActivatorReceiver::~ActivatorReceiver (void)
{
}

void
ActivatorReceiver::start_server (void)
{
  this->aam_->activator_replied (true);
  PortableServer::ObjectId_var oid = this->poa_->servant_to_id (this);
  poa_->deactivate_object (oid.in());
}

void
ActivatorReceiver::start_server_excep (Messaging::ExceptionHolder *)
{
  this->aam_->activator_replied (false);
  PortableServer::ObjectId_var oid = this->poa_->servant_to_id (this);
  poa_->deactivate_object (oid.in());
}

void
ActivatorReceiver::shutdown (void)
{
  // no-op, just satisfy virtual function
}

void
ActivatorReceiver::shutdown_excep (Messaging::ExceptionHolder * )
{
  // no-op, just satisfy virtual function
}

void
ActivatorReceiver::kill_server (CORBA::Boolean )
{
  // no-op, just satisfy virtual function
}

void
ActivatorReceiver::kill_server_excep (Messaging::ExceptionHolder * )
{
  // no-op, just satisfy virtual function
}


//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

AccessLiveListener::AccessLiveListener (const char *server,
                                      AsyncAccessManager *aam,
                                      LiveCheck &pinger)
  :LiveListener (server),
   aam_ (aam->_add_ref ()),
   pinger_ (pinger),
   status_ (LS_UNKNOWN),
   per_client_ (false),
   srv_ref_ (ImplementationRepository::ServerObject::_nil())
{
}

AccessLiveListener::AccessLiveListener (const char *server,
                                      AsyncAccessManager *aam,
                                      LiveCheck &pinger,
                                      ImplementationRepository::ServerObject_ptr ref)
  :LiveListener (server),
   aam_ (aam->_add_ref ()),
   pinger_ (pinger),
   status_ (LS_UNKNOWN),
   per_client_ (true),
   srv_ref_ (ImplementationRepository::ServerObject::_duplicate (ref))
{
}

AccessLiveListener::~AccessLiveListener (void)
{
}

bool
AccessLiveListener::start (void)
{
  bool rtn = this->per_client_ ?
    this->pinger_.add_per_client_listener (this, srv_ref_.in()) :
    this->pinger_.add_listener (this);
  return rtn;
}

bool
AccessLiveListener::status_changed (LiveStatus status)
{
  this->status_ = status;
  if (status == LS_TRANSIENT)
    {
      return false;
    }
  else
    {
      this->aam_->ping_replied (status);
    }
  return true;
}
