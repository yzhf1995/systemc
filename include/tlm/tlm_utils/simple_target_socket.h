/*****************************************************************************

  The following code is derived, directly or indirectly, from the SystemC
  source code Copyright (c) 1996-2007 by all Contributors.
  All Rights reserved.

  The contents of this file are subject to the restrictions and limitations
  set forth in the SystemC Open Source License Version 3.0 (the "License");
  You may not use this file except in compliance with such restrictions and
  limitations. You may obtain instructions on how to receive a copy of the
  License at http://www.systemc.org/. Software distributed by Contributors
  under the License is distributed on an "AS IS" basis, WITHOUT WARRANTY OF
  ANY KIND, either express or implied. See the License for the specific
  language governing rights and limitations under the License.

 *****************************************************************************/

#ifndef __SIMPLE_TARGET_SOCKET_H__
#define __SIMPLE_TARGET_SOCKET_H__

#include "tlm.h"
#include "peq_with_get.h"

namespace tlm_utils {

template <typename MODULE,
          unsigned int BUSWIDTH = 32,
          typename TYPES = tlm::tlm_generic_payload_types>
class simple_target_socket :
  public tlm::tlm_target_socket<BUSWIDTH, TYPES>
{
  friend class fw_process;
  friend class bw_process;
public:
  typedef typename TYPES::tlm_payload_type              transaction_type;
  typedef typename TYPES::tlm_phase_type                phase_type;
  typedef tlm::tlm_sync_enum                            sync_enum_type;
  typedef tlm::tlm_fw_transport_if<TYPES>               fw_interface_type;
  typedef tlm::tlm_bw_transport_if<TYPES>               bw_interface_type;
  typedef tlm::tlm_target_socket<BUSWIDTH, TYPES>       base_type;

public:
  explicit simple_target_socket(const char* n = "simple_target_socket") :
    base_type(sc_core::sc_gen_unique_name(n)),
    m_fw_process(this),
    m_bw_process(this)
  {
    bind(m_fw_process);
  }

  // bw transport must come thru us.
  tlm::tlm_bw_transport_if<TYPES> * operator ->() {return &m_bw_process;}

  // REGISTER_XXX
  void register_nb_transport_fw(MODULE* mod,
                                sync_enum_type (MODULE::*cb)(transaction_type&,
                                                             phase_type&,
                                                             sc_core::sc_time&))
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_nb_transport_ptr(mod, cb);
  }

  void register_b_transport(MODULE* mod,
                            void (MODULE::*cb)(transaction_type&,
                                               sc_core::sc_time&))
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_b_transport_ptr(mod, cb);
  }

  void register_transport_dbg(MODULE* mod,
                              unsigned int (MODULE::*cb)(transaction_type&))
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_transport_dbg_ptr(mod, cb);
  }

  void register_get_direct_mem_ptr(MODULE* mod,
                                   bool (MODULE::*cb)(transaction_type&,
                                                      tlm::tlm_dmi&))
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_get_direct_mem_ptr(mod, cb);
  }

private:
  //make call on bw path.
  sync_enum_type bw_nb_transport(transaction_type &trans, phase_type &phase, sc_core::sc_time &t)
  {
    return base_type::operator ->()->nb_transport_bw(trans, phase, t);
  }

  void bw_invalidate_direct_mem_ptr(sc_dt::uint64 s,sc_dt::uint64 e)
  {
    base_type::operator ->()->invalidate_direct_mem_ptr(s, e);
  }

  //Helper class to handle bw path calls
  // Needed to detect transaction end when called from b_transport.
  class bw_process : public tlm::tlm_bw_transport_if<TYPES>
  {
  public:
    bw_process(simple_target_socket *p_own) : m_owner(p_own)
    {
    }

    sync_enum_type nb_transport_bw(transaction_type &trans, phase_type &phase, sc_core::sc_time &t)
    {
      typename std::map<transaction_type*, sc_core::sc_event *>::iterator it;
      
      it = m_owner->m_pending_trans.find(&trans);
      if(it == m_owner->m_pending_trans.end()) {
        // Not a blocking call, forward.
        return m_owner->bw_nb_transport(trans, phase, t);

      } else {
        if (phase == tlm::END_REQ) {
          m_owner->m_end_request.notify(sc_core::SC_ZERO_TIME);
          return tlm::TLM_ACCEPTED;
        
        } else if (phase == tlm::BEGIN_RESP) {
          if (m_owner->m_current_transaction == &trans) {
            m_owner->m_end_request.notify(sc_core::SC_ZERO_TIME);
          }
          //TODO: add response-accept delay?
          it->second->notify(t);
          m_owner->m_pending_trans.erase(it);
          return tlm::TLM_COMPLETED;

        } else {
          assert(0); exit(1);
        }

//        return tlm::TLM_COMPLETED;  //Should not reach here
      }
    }

    void invalidate_direct_mem_ptr(sc_dt::uint64 s,sc_dt::uint64 e)
    {
      return m_owner->bw_invalidate_direct_mem_ptr(s, e);
    }

  private:
    simple_target_socket *m_owner;
  };

  class fw_process : public tlm::tlm_fw_transport_if<TYPES>,
                    public tlm::tlm_mm_interface
  {
  public:
    typedef sync_enum_type (MODULE::*NBTransportPtr)(transaction_type&,
                                                     tlm::tlm_phase&,
                                                     sc_core::sc_time&);
    typedef void (MODULE::*BTransportPtr)(transaction_type&,
                                          sc_core::sc_time&);
    typedef unsigned int (MODULE::*TransportDbgPtr)(transaction_type&);
    typedef bool (MODULE::*GetDirectMemPtr)(transaction_type&,
                                            tlm::tlm_dmi&);
      
    fw_process(simple_target_socket *p_own) :
      m_name(p_own->name()),
      m_owner(p_own),
      m_mod(0),
      m_nb_transport_ptr(0),
      m_b_transport_ptr(0),
      m_transport_dbg_ptr(0),
      m_get_direct_mem_ptr(0),
      m_peq(sc_core::sc_gen_unique_name("m_peq")),
      m_response_in_progress(false)
    {
      sc_core::sc_spawn_options opts;
      opts.set_sensitivity(&m_peq.get_event());
      sc_core::sc_spawn(sc_bind(&fw_process::b2nb_thread, this), 
                        sc_core::sc_gen_unique_name("b2nb_thread"), &opts);
    }
  
    void set_nb_transport_ptr(MODULE* mod, NBTransportPtr p)
    {
      if (m_nb_transport_ptr) {
        std::cerr << m_name << ": non-blocking callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_nb_transport_ptr = p;
      }
    }

    void set_b_transport_ptr(MODULE* mod, BTransportPtr p)
    {
      if (m_b_transport_ptr) {
        std::cerr << m_name << ": non-blocking callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_b_transport_ptr = p;
      }
    }

    void set_transport_dbg_ptr(MODULE* mod, TransportDbgPtr p)
    {
      if (m_transport_dbg_ptr) {
        std::cerr << m_name << ": debug callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_transport_dbg_ptr = p;
      }
    }

    void set_get_direct_mem_ptr(MODULE* mod, GetDirectMemPtr p)
    {
      if (m_get_direct_mem_ptr) {
        std::cerr << m_name << ": get DMI pointer callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_get_direct_mem_ptr = p;
      }
    }
// Interface implementation
    sync_enum_type nb_transport_fw(transaction_type& trans,
                                   phase_type& phase,
                                   sc_core::sc_time& t)
    {
      if (m_nb_transport_ptr) {
        // forward call
        assert(m_mod);
        return (m_mod->*m_nb_transport_ptr)(trans, phase, t);

      } else if (m_b_transport_ptr) {
        if (phase == tlm::BEGIN_REQ) {
          // create thread to do blocking call
          sc_core::sc_spawn_options opts;
          opts.dont_initialize();
          sc_core::sc_event *e = new sc_core::sc_event;
          opts.set_sensitivity(e);
          sc_core::sc_spawn(sc_bind(&fw_process::nb2b_thread, this, sc_ref(trans), e), 
                            sc_core::sc_gen_unique_name("nb2b_thread"), &opts);
          e->notify(t);
          return tlm::TLM_ACCEPTED;

        } else if (phase == tlm::END_RESP) {
          m_response_in_progress = false;
          m_end_response.notify(t);
          return tlm::TLM_COMPLETED;

        } else {
          assert(0); exit(1);
//          return tlm::TLM_COMPLETED;   ///< unreachable code
        }

      } else {
        std::cerr << m_name << ": no transport callback registered" << std::endl;
        assert(0); exit(1);
//        return tlm::TLM_COMPLETED;   ///< unreachable code
      }
    }

    void b_transport(transaction_type& trans, sc_core::sc_time& t)
    {
      if (m_b_transport_ptr) {
        // forward call
        assert(m_mod);
        (m_mod->*m_b_transport_ptr)(trans, t);
        return;
      
      } else if (m_nb_transport_ptr) {
        m_peq.notify(trans, t);
        t = sc_core::SC_ZERO_TIME;

        mm_end_event_ext mm_ext;
        const bool mm_added = !trans.has_mm();

        if (mm_added) {
          trans.set_mm(this);
          trans.set_auto_extension(&mm_ext);
          trans.acquire();
        }

        // wait until transaction is finished
        sc_core::sc_event end_event;
        m_owner->m_pending_trans[&trans] = &end_event;
        sc_core::wait(end_event);

        if (mm_added) {
          // release will not delete the transaction, it will notify mm_ext.done
          trans.release();
          if (trans.get_ref_count()) {
            sc_core::wait(mm_ext.done);
          }
          trans.set_mm(0);
        }

      } else {
        std::cerr << m_name << ": no transport callback registered" << std::endl;
        assert(0); exit(1);
//        return tlm::TLM_COMPLETED;   ///< unreachable code
      }
    }

    unsigned int transport_dbg(transaction_type& trans)
    {
      if (m_transport_dbg_ptr) {
        // forward call
        assert(m_mod);
        return (m_mod->*m_transport_dbg_ptr)(trans);

      } else {
        // No debug support
        return 0;
      }
    }

    bool get_direct_mem_ptr(transaction_type& trans,
                            tlm::tlm_dmi&  dmi_data)
    {
      if (m_get_direct_mem_ptr) {
        // forward call
        assert(m_mod);
        return (m_mod->*m_get_direct_mem_ptr)(trans, dmi_data);

      } else {
        // No DMI support
        dmi_data.allow_read_write();
        dmi_data.set_start_address(0x0);
        dmi_data.set_end_address((sc_dt::uint64)-1);
        return false;
      }
    }

  private:
    void nb2b_thread(transaction_type& trans, sc_core::sc_event* e)
    {
      sc_core::sc_time t = sc_core::SC_ZERO_TIME;

      // forward call
      assert(m_mod);
      (m_mod->*m_b_transport_ptr)(trans, t);

      sc_core::wait(t);

      // return path
      while (m_response_in_progress) {
        sc_core::wait(m_end_response);
      }
      t = sc_core::SC_ZERO_TIME;
      phase_type phase = tlm::BEGIN_RESP;
      if (m_owner->bw_nb_transport(trans, phase, t) != tlm::TLM_COMPLETED) {
        m_response_in_progress = true;
      }

      // cleanup
      delete e;
    }

    void b2nb_thread()
    {
      while (true) {
        sc_core::wait(m_peq.get_event());

        transaction_type* trans;
        while ((trans = m_peq.get_next_transaction())!=0) {
          assert(m_mod);
          assert(m_nb_transport_ptr);
          phase_type phase = tlm::BEGIN_REQ;
          sc_core::sc_time t = sc_core::SC_ZERO_TIME;

          switch ((m_mod->*m_nb_transport_ptr)(*trans, phase, t)) {
          case tlm::TLM_COMPLETED:
          {
            // notify transaction is finished
            typename std::map<transaction_type*, sc_core::sc_event *>::iterator it =
              m_owner->m_pending_trans.find(trans);
            assert(it != m_owner->m_pending_trans.end());
            it->second->notify(t);
            m_owner->m_pending_trans.erase(it);
            break;
          }

          case tlm::TLM_ACCEPTED:
          case tlm::TLM_UPDATED:
            switch (phase) {
            case tlm::BEGIN_REQ:
              m_owner->m_current_transaction = trans;
              sc_core::wait(m_owner->m_end_request);
              m_owner->m_current_transaction = 0;
              break;

            case tlm::END_REQ:
              sc_core::wait(t);
              break;

            case tlm::BEGIN_RESP:
            {
              phase = tlm::END_RESP;
              t = sc_core::SC_ZERO_TIME;
              (m_mod->*m_nb_transport_ptr)(*trans, phase, t);
          
              // notify transaction is finished
              typename std::map<transaction_type*, sc_core::sc_event *>::iterator it =
                m_owner->m_pending_trans.find(trans);
              assert(it != m_owner->m_pending_trans.end());
              it->second->notify(t);
              m_owner->m_pending_trans.erase(it);
              break;
            }

            default:
              assert(0); exit(1);
            };
            break;

          default:
            assert(0); exit(1);
          };
        }
      }
    }

    void free(tlm::tlm_generic_payload* trans)
    {
      mm_end_event_ext* ext = trans->template get_extension<mm_end_event_ext>();
      assert(ext);
      // notif event first before freeing extensions (reset)
      ext->done.notify();
      trans->reset();
    }

  private:
    struct mm_end_event_ext : public tlm::tlm_extension<mm_end_event_ext>
    {
      tlm::tlm_extension_base* clone() const { return NULL; }
      void free() {}
      void copy_from(tlm::tlm_extension_base const &) {}
      sc_core::sc_event done;
    };

  private:
    const std::string m_name;
    simple_target_socket *m_owner;
    MODULE* m_mod;
    NBTransportPtr m_nb_transport_ptr;
    BTransportPtr m_b_transport_ptr;
    TransportDbgPtr m_transport_dbg_ptr;
    GetDirectMemPtr m_get_direct_mem_ptr;
    peq_with_get<transaction_type> m_peq;
    bool m_response_in_progress;
    sc_core::sc_event m_end_response;
  };

private:
  fw_process m_fw_process;
  bw_process m_bw_process;
  std::map<transaction_type*, sc_core::sc_event *> m_pending_trans;
  sc_core::sc_event m_end_request;
  transaction_type* m_current_transaction;
};

//ID Tagged version
template <typename MODULE,
          unsigned int BUSWIDTH = 32,
          typename TYPES = tlm::tlm_generic_payload_types>
class simple_target_socket_tagged :
  public tlm::tlm_target_socket<BUSWIDTH, TYPES>
{
  friend class fw_process;
  friend class bw_process;
public:
  typedef typename TYPES::tlm_payload_type              transaction_type;
  typedef typename TYPES::tlm_phase_type                phase_type;
  typedef tlm::tlm_sync_enum                            sync_enum_type;
  typedef tlm::tlm_fw_transport_if<TYPES>               fw_interface_type;
  typedef tlm::tlm_bw_transport_if<TYPES>               bw_interface_type;
  typedef tlm::tlm_target_socket<BUSWIDTH, TYPES>       base_type;

public:
  explicit simple_target_socket_tagged(const char* n = "simple_target_socket_tagged") :
    base_type(sc_core::sc_gen_unique_name(n)),
    m_fw_process(this),
    m_bw_process(this)
  {
    bind(m_fw_process);
  }

  // bw transport must come thru us.
  tlm::tlm_bw_transport_if<TYPES> * operator ->() {return &m_bw_process;}

  // REGISTER_XXX
  void register_nb_transport_fw(MODULE* mod,
                                sync_enum_type (MODULE::*cb)(int id,
                                                             transaction_type&,
                                                             phase_type&,
                                                             sc_core::sc_time&),
                                int id)
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_nb_transport_ptr(mod, cb);
    m_fw_process.set_nb_transport_user_id(id);
  }

  void register_b_transport(MODULE* mod,
                            void (MODULE::*cb)(int id,
                                               transaction_type&,
                                               sc_core::sc_time&),
                            int id)
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_b_transport_ptr(mod, cb);
    m_fw_process.set_b_transport_user_id(id);
  }

  void register_transport_dbg(MODULE* mod,
                              unsigned int (MODULE::*cb)(int id,
                                                         transaction_type&),
                              int id)
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_transport_dbg_ptr(mod, cb);
    m_fw_process.set_transport_dbg_user_id(id);
  }

  void register_get_direct_mem_ptr(MODULE* mod,
                                   bool (MODULE::*cb)(int id,
                                                      transaction_type&,
                                                      tlm::tlm_dmi&),
                                   int id)
  {
    assert(!sc_core::sc_get_curr_simcontext()->elaboration_done());
    m_fw_process.set_get_direct_mem_ptr(mod, cb);
    m_fw_process.set_get_dmi_user_id(id);
  }

private:
  //make call on bw path.
  sync_enum_type bw_nb_transport(transaction_type &trans, phase_type &phase, sc_core::sc_time &t)
  {
    return base_type::operator ->()->nb_transport_bw(trans, phase, t);
  }

  void bw_invalidate_direct_mem_ptr(sc_dt::uint64 s,sc_dt::uint64 e)
  {
    base_type::operator ->()->invalidate_direct_mem_ptr(s, e);
  }

  //Helper class to handle bw path calls
  // Needed to detect transaction end when called from b_transport.
  class bw_process : public tlm::tlm_bw_transport_if<TYPES>
  {
  public:
    bw_process(simple_target_socket_tagged *p_own) : m_owner(p_own)
    {
    }

    sync_enum_type nb_transport_bw(transaction_type &trans, phase_type &phase, sc_core::sc_time &t)
    {
      typename std::map<transaction_type*, sc_core::sc_event *>::iterator it;
      
      it = m_owner->m_pending_trans.find(&trans);
      if(it == m_owner->m_pending_trans.end()) {
        // Not a blocking call, forward.
        return m_owner->bw_nb_transport(trans, phase, t);

      } else {
        if (phase == tlm::END_REQ) {
          m_owner->m_end_request.notify(sc_core::SC_ZERO_TIME);
          return tlm::TLM_ACCEPTED;
        
        } else if (phase == tlm::BEGIN_RESP) {
          if (m_owner->m_current_transaction == &trans) {
            m_owner->m_end_request.notify(sc_core::SC_ZERO_TIME);
          }
          //TODO: add response-accept delay?
          it->second->notify(t);
          m_owner->m_pending_trans.erase(it);
          return tlm::TLM_COMPLETED;

        } else {
          assert(0); exit(1);
        }

//        return tlm::TLM_COMPLETED;  //Should not reach here
      }
    }

    void invalidate_direct_mem_ptr(sc_dt::uint64 s,sc_dt::uint64 e)
    {
      return m_owner->bw_invalidate_direct_mem_ptr(s, e);
    }

  private:
    simple_target_socket_tagged *m_owner;
  };

  class fw_process : public tlm::tlm_fw_transport_if<TYPES>,
                     public tlm::tlm_mm_interface
  {
  public:
    typedef sync_enum_type (MODULE::*NBTransportPtr)(int id, 
                                                     transaction_type&,
                                                     tlm::tlm_phase&,
                                                     sc_core::sc_time&);
    typedef void (MODULE::*BTransportPtr)(int id, 
                                          transaction_type&,
                                          sc_core::sc_time&);
    typedef unsigned int (MODULE::*TransportDbgPtr)(int id, 
                                                    transaction_type&);
    typedef bool (MODULE::*GetDirectMemPtr)(int id, 
                                            transaction_type&,
                                            tlm::tlm_dmi&);
      
    fw_process(simple_target_socket_tagged *p_own) :
      m_name(p_own->name()),
      m_owner(p_own),
      m_mod(0),
      m_nb_transport_ptr(0),
      m_b_transport_ptr(0),
      m_transport_dbg_ptr(0),
      m_get_direct_mem_ptr(0),
      m_nb_transport_user_id(0),
      m_b_transport_user_id(0),
      m_transport_dbg_user_id(0),
      m_get_dmi_user_id(0),
      m_peq(sc_core::sc_gen_unique_name("m_peq")),
      m_response_in_progress(false)
    {
      sc_core::sc_spawn_options opts;
      opts.set_sensitivity(&m_peq.get_event());
      sc_core::sc_spawn(sc_bind(&fw_process::b2nb_thread, this), 
                        sc_core::sc_gen_unique_name("b2nb_thread"), &opts);
    }
  
    void set_nb_transport_user_id(int id) { m_nb_transport_user_id = id; }
    void set_b_transport_user_id(int id) { m_b_transport_user_id = id; }
    void set_transport_dbg_user_id(int id) { m_transport_dbg_user_id = id; }
    void set_get_dmi_user_id(int id) { m_get_dmi_user_id = id; }

    void set_nb_transport_ptr(MODULE* mod, NBTransportPtr p)
    {
      if (m_nb_transport_ptr) {
        std::cerr << m_name << ": non-blocking callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_nb_transport_ptr = p;
      }
    }

    void set_b_transport_ptr(MODULE* mod, BTransportPtr p)
    {
      if (m_b_transport_ptr) {
        std::cerr << m_name << ": non-blocking callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_b_transport_ptr = p;
      }
    }

    void set_transport_dbg_ptr(MODULE* mod, TransportDbgPtr p)
    {
      if (m_transport_dbg_ptr) {
        std::cerr << m_name << ": debug callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_transport_dbg_ptr = p;
      }
    }

    void set_get_direct_mem_ptr(MODULE* mod, GetDirectMemPtr p)
    {
      if (m_get_direct_mem_ptr) {
        std::cerr << m_name << ": get DMI pointer callback allready registered" << std::endl;

      } else {
        assert(!m_mod || m_mod == mod);
        m_mod = mod;
        m_get_direct_mem_ptr = p;
      }
    }
// Interface implementation
    sync_enum_type nb_transport_fw(transaction_type& trans,
                                   phase_type& phase,
                                   sc_core::sc_time& t)
    {
      if (m_nb_transport_ptr) {
        // forward call
        assert(m_mod);
        return (m_mod->*m_nb_transport_ptr)(m_nb_transport_user_id, trans, phase, t);

      } else if (m_b_transport_ptr) {
        if (phase == tlm::BEGIN_REQ) {
          // create thread to do blocking call
          sc_core::sc_spawn_options opts;
          opts.dont_initialize();
          sc_core::sc_event *e = new sc_core::sc_event;
          opts.set_sensitivity(e);
          sc_core::sc_spawn(sc_bind(&fw_process::nb2b_thread, this, sc_ref(trans), e), 
                            sc_core::sc_gen_unique_name("nb2b_thread"), &opts);
          e->notify(t);
          return tlm::TLM_ACCEPTED;

        } else if (phase == tlm::END_RESP) {
          m_response_in_progress = false;
          m_end_response.notify(t);
          return tlm::TLM_COMPLETED;

        } else {
          assert(0); exit(1);
//          return tlm::TLM_COMPLETED;   ///< unreachable code
        }

      } else {
        std::cerr << m_name << ": no transport callback registered" << std::endl;
        assert(0); exit(1);
//        return tlm::TLM_COMPLETED;   ///< unreachable code
      }
    }

    void b_transport(transaction_type& trans, sc_core::sc_time& t)
    {
      if (m_b_transport_ptr) {
        // forward call
        assert(m_mod);
        (m_mod->*m_b_transport_ptr)(m_b_transport_user_id, trans, t);
        return;
      
      } else if (m_nb_transport_ptr) {
        m_peq.notify(trans, t);
        t = sc_core::SC_ZERO_TIME;

        mm_end_event_ext mm_ext;
        const bool mm_added = !trans.has_mm();

        if (mm_added){
          trans.set_mm(this);
          trans.set_auto_extension(&mm_ext);
          trans.acquire();
        }

        // wait until transaction is finished
        sc_core::sc_event end_event;
        m_owner->m_pending_trans[&trans] = &end_event;
        sc_core::wait(end_event);

        if (mm_added) {
          // release will not delete the transaction, it will notify mm_ext.done
          trans.release();
          if (trans.get_ref_count()) {
            sc_core::wait(mm_ext.done);
          }
          trans.set_mm(0);
        }

      } else {
        std::cerr << m_name << ": no transport callback registered" << std::endl;
        assert(0); exit(1);
//        return tlm::TLM_COMPLETED;   ///< unreachable code
      }
    }

    unsigned int transport_dbg(transaction_type& trans)
    {
      if (m_transport_dbg_ptr) {
        // forward call
        assert(m_mod);
        return (m_mod->*m_transport_dbg_ptr)(m_transport_dbg_user_id, trans);

      } else {
        // No debug support
        return 0;
      }
    }

    bool get_direct_mem_ptr(transaction_type& trans,
                            tlm::tlm_dmi&  dmi_data)
    {
      if (m_get_direct_mem_ptr) {
        // forward call
        assert(m_mod);
        return (m_mod->*m_get_direct_mem_ptr)(m_get_dmi_user_id, trans, dmi_data);

      } else {
        // No DMI support
        dmi_data.allow_read_write();
        dmi_data.set_start_address(0x0);
        dmi_data.set_end_address((sc_dt::uint64)-1);
        return false;
      }
    }

  private:
    void nb2b_thread(transaction_type& trans, sc_core::sc_event* e)
    {
      sc_core::sc_time t = sc_core::SC_ZERO_TIME;

      // forward call
      assert(m_mod);
      (m_mod->*m_b_transport_ptr)(m_b_transport_user_id, trans, t);

      sc_core::wait(t);

      // return path
      while (m_response_in_progress) {
        sc_core::wait(m_end_response);
      }
      t = sc_core::SC_ZERO_TIME;
      phase_type phase = tlm::BEGIN_RESP;
      if (m_owner->bw_nb_transport(trans, phase, t) != tlm::TLM_COMPLETED) {
        m_response_in_progress = true;
      }

      // cleanup
      delete e;
    }

    void b2nb_thread()
    {
      while (true) {
        sc_core::wait(m_peq.get_event());

        transaction_type* trans;
        while ((trans = m_peq.get_next_transaction())!=0) {
          assert(m_mod);
          assert(m_nb_transport_ptr);
          phase_type phase = tlm::BEGIN_REQ;
          sc_core::sc_time t = sc_core::SC_ZERO_TIME;

          switch ((m_mod->*m_nb_transport_ptr)(m_nb_transport_user_id, *trans, phase, t)) {
          case tlm::TLM_COMPLETED:
          {
            // notify transaction is finished
            typename std::map<transaction_type*, sc_core::sc_event *>::iterator it =
              m_owner->m_pending_trans.find(trans);
            assert(it != m_owner->m_pending_trans.end());
            it->second->notify(t);
            m_owner->m_pending_trans.erase(it);
            break;
          }

          case tlm::TLM_ACCEPTED:
          case tlm::TLM_UPDATED:
            switch (phase) {
            case tlm::BEGIN_REQ:
              m_owner->m_current_transaction = trans;
              sc_core::wait(m_owner->m_end_request);
              m_owner->m_current_transaction = 0;
              break;

            case tlm::END_REQ:
              sc_core::wait(t);
              break;

            case tlm::BEGIN_RESP:
            {
              phase = tlm::END_RESP;
              t = sc_core::SC_ZERO_TIME;
              (m_mod->*m_nb_transport_ptr)(m_nb_transport_user_id, *trans, phase, t);
          
              // notify transaction is finished
              typename std::map<transaction_type*, sc_core::sc_event *>::iterator it =
                m_owner->m_pending_trans.find(trans);
              assert(it != m_owner->m_pending_trans.end());
              it->second->notify(t);
              m_owner->m_pending_trans.erase(it);
              break;
            }

            default:
              assert(0); exit(1);
            };
            break;

          default:
            assert(0); exit(1);
          };
        }
      }
    }

    void free(tlm::tlm_generic_payload* trans)
    {
      mm_end_event_ext* ext = trans->template get_extension<mm_end_event_ext>();
      assert(ext);
      // notif event first before freeing extensions (reset)
      ext->done.notify();
      trans->reset();
    }

  private:
    struct mm_end_event_ext : public tlm::tlm_extension<mm_end_event_ext>
    {
      tlm::tlm_extension_base* clone() const { return NULL; }
      void free() {}
      void copy_from(tlm::tlm_extension_base const &) {}
      sc_core::sc_event done;
    };

  private:
    const std::string m_name;
    simple_target_socket_tagged *m_owner;
    MODULE* m_mod;
    NBTransportPtr m_nb_transport_ptr;
    BTransportPtr m_b_transport_ptr;
    TransportDbgPtr m_transport_dbg_ptr;
    GetDirectMemPtr m_get_direct_mem_ptr;
    int m_nb_transport_user_id;
    int m_b_transport_user_id;
    int m_transport_dbg_user_id;
    int m_get_dmi_user_id;
    peq_with_get<transaction_type> m_peq;
    bool m_response_in_progress;
    sc_core::sc_event m_end_response;
  };

private:
  fw_process m_fw_process;
  bw_process m_bw_process;
  std::map<transaction_type*, sc_core::sc_event *> m_pending_trans;
  sc_core::sc_event m_end_request;
  transaction_type* m_current_transaction;
};

}

#endif
