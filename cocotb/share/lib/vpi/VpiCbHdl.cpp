/******************************************************************************
* Copyright (c) 2013, 2018 Potential Ventures Ltd
* Copyright (c) 2013 SolarFlare Communications Inc
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright
*      notice, this list of conditions and the following disclaimer in the
*      documentation and/or other materials provided with the distribution.
*    * Neither the name of Potential Ventures Ltd,
*       SolarFlare Communications Inc nor the
*      names of its contributors may be used to endorse or promote products
*      derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL POTENTIAL VENTURES LTD BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include <cstdlib>

#include "VpiImpl.h"

extern "C" int32_t handle_vpi_callback(p_cb_data cb_data);

VpiCbHdl::VpiCbHdl(GpiImplInterface *impl) : GpiCbHdl(impl)
{

    vpi_time.high = 0;
    vpi_time.low = 0;
    vpi_time.type = vpiSimTime;

    cb_data.reason    = 0;
    cb_data.cb_rtn    = handle_vpi_callback;
    cb_data.obj       = NULL;
    cb_data.time      = &vpi_time;
    cb_data.value     = NULL;
    cb_data.index     = 0;
    cb_data.user_data = (char*)this;
}

/* If the user data already has a callback handle then deregister
 * before getting the new one
 */
int VpiCbHdl::arm_callback(void) {

    if (m_state == GPI_PRIMED) {
        fprintf(stderr,
                "Attempt to prime an already primed trigger for %s!\n",
                m_impl->reason_to_string(cb_data.reason));
    }

    // Only a problem if we have not been asked to deregister and register
    // in the same simulation callback
    if (m_obj_hdl != NULL && m_state != GPI_DELETE) {
        fprintf(stderr,
                "We seem to already be registered, deregistering %s!\n",
                m_impl->reason_to_string(cb_data.reason));
        cleanup_callback();
    }

    vpiHandle new_hdl = vpi_register_cb(&cb_data);

    if (!new_hdl) {
        LOG_ERROR("VPI: Unable to register a callback handle for VPI type %s(%d)",
                  m_impl->reason_to_string(cb_data.reason), cb_data.reason);
        check_vpi_error();
        return -1;

    } else {
        m_state = GPI_PRIMED;
    }

    m_obj_hdl = new_hdl;

    return 0;
}

int VpiCbHdl::cleanup_callback(void)
{
    if (m_state == GPI_FREE)
        return 0;

    /* If the one-time callback has not come back then
     * remove it, it is has then free it. The remove is done
     * internally */

    if (m_state == GPI_PRIMED) {
        if (!m_obj_hdl) {
            LOG_CRITICAL("VPI: passed a NULL pointer : ABORTING");
        }

        if (!(vpi_remove_cb(get_handle<vpiHandle>()))) {
            LOG_CRITICAL("VPI: unable to remove callback : ABORTING");
        }

        check_vpi_error();
    } else {
#ifndef MODELSIM
        /* This is disabled for now, causes a small leak going to put back in */
        if (!(vpi_free_object(get_handle<vpiHandle>()))) {
            LOG_CRITICAL("VPI: unable to free handle : ABORTING");
        }
#endif
    }


    m_obj_hdl = NULL;
    m_state = GPI_FREE;

    return 0;
}

int VpiPseudoGenArrayObjHdl::initialise(GpiObjHdlId &id) {
    return GpiPseudoObjHdl::initialise(id);
}

int VpiPseudoArrayObjHdl::initialise(GpiObjHdlId &id) {
    int range_idx   = 1;  // At least one index counting this one
    GpiObjHdl *current = get_parent();

    while (current->is_pseudo()) {
        ++range_idx;
        current = current->get_parent();
    }

    vpiHandle hdl = current->get_handle<vpiHandle>();

    /* After determining the range_idx, get the range and set the limits */
    vpiHandle iter = vpi_iterate(vpiRange, hdl);

    s_vpi_value val;
    val.format = vpiIntVal;

    if (iter != NULL) {
        vpiHandle rangeHdl;
        int idx = 0;

        while ((rangeHdl = vpi_scan(iter)) != NULL) {
            if (idx == range_idx) {
                break;
            }
            ++idx;
        }

        if (rangeHdl == NULL) {
            LOG_CRITICAL("Unable to get range for indexable object");
        } else {
            vpi_free_object(iter); // Need to free iterator since exited early

            vpi_get_value(vpi_handle(vpiLeftRange,rangeHdl),&val);
            check_vpi_error();
            m_range_left = val.value.integer;

            vpi_get_value(vpi_handle(vpiRightRange,rangeHdl),&val);
            check_vpi_error();
            m_range_right = val.value.integer;
        }
    } else {
        LOG_CRITICAL("Unable to get range for indexable object");
        return -1;
    }

    /* vpiSize will return a size that is incorrect for multi-dimensional arrays so use the range
     * to calculate the m_num_elems.
     *
     *    For example:
     *       wire [7:0] sig_t4 [0:3][7:4]
     *
     *    The size of "sig_t4" will be reported as 16 through the vpi interface.
     */
    if (m_range_left > m_range_right) {
        m_num_elems = m_range_left - m_range_right + 1;
    } else {
        m_num_elems = m_range_right - m_range_left + 1;
    }

    return GpiPseudoObjHdl::initialise(id);
}

int VpiArrayObjHdl::initialise(GpiObjHdlId &id) {
    vpiHandle hdl = GpiObjHdl::get_handle<vpiHandle>();

    m_indexable   = true;

    vpiHandle iter = vpi_iterate(vpiRange, hdl);

    s_vpi_value val;
    val.format = vpiIntVal;

    if (iter != NULL) {
        vpiHandle rangeHdl;

        rangeHdl = vpi_scan(iter);

        if (rangeHdl == NULL) {
            LOG_CRITICAL("Unable to get range for indexable object");
            return -1;
        } else {
            vpi_free_object(iter); // Need to free iterator since exited early

            vpi_get_value(vpi_handle(vpiLeftRange,rangeHdl),&val);
            check_vpi_error();
            m_range_left = val.value.integer;

            vpi_get_value(vpi_handle(vpiRightRange,rangeHdl),&val);
            check_vpi_error();
            m_range_right = val.value.integer;
        }
    } else {
        vpi_get_value(vpi_handle(vpiLeftRange,hdl),&val);
        check_vpi_error();
        m_range_left = val.value.integer;

        vpi_get_value(vpi_handle(vpiRightRange,hdl),&val);
        check_vpi_error();
        m_range_right = val.value.integer;
    }

    /* vpiSize will return a size that is incorrect for multi-dimensional arrays so use the range
     * to calculate the m_num_elems.
     *
     *    For example:
     *       wire [7:0] sig_t4 [0:3][7:4]
     *
     *    The size of "sig_t4" will be reported as 16 through the vpi interface.
     */
    if (m_range_left > m_range_right) {
        m_num_elems = m_range_left - m_range_right + 1;
    } else {
        m_num_elems = m_range_right - m_range_left + 1;
    }

    return GpiObjHdl::initialise(id);
}

int VpiObjHdl::initialise(GpiObjHdlId &id) {
    char * str;
    vpiHandle hdl = GpiObjHdl::get_handle<vpiHandle>();
    str = vpi_get_str(vpiDefName, hdl);
    if (str != NULL)
        m_definition_name = str;
    str = vpi_get_str(vpiDefFile, hdl);
    if (str != NULL)
        m_definition_file = str;

    return GpiObjHdl::initialise(id);
}

int VpiSignalObjHdl::initialise(GpiObjHdlId &id) {
    int32_t type = vpi_get(vpiType, GpiObjHdl::get_handle<vpiHandle>());
    if ((vpiIntVar == type) ||
        (vpiIntegerVar == type) ||
        (vpiIntegerNet == type )) {
        m_num_elems = 1;
    } else {
        m_num_elems = vpi_get(vpiSize, GpiObjHdl::get_handle<vpiHandle>());

        if (GpiObjHdl::get_type() == GPI_STRING) {
            m_indexable   = false; // Don't want to iterate over indices
            m_range_left  = 0;
            m_range_right = m_num_elems-1;
        } else if (GpiObjHdl::get_type() == GPI_REGISTER) {
            vpiHandle hdl = GpiObjHdl::get_handle<vpiHandle>();

            m_indexable   = vpi_get(vpiVector, hdl);

            if (m_indexable) {
                s_vpi_value val;
                vpiHandle iter;

                val.format = vpiIntVal;

                iter = vpi_iterate(vpiRange, hdl);

                /* Only ever need the first "range" */
                if (iter != NULL) {
                    vpiHandle rangeHdl = vpi_scan(iter);

                    vpi_free_object(iter);

                    if (rangeHdl != NULL) {
                        vpi_get_value(vpi_handle(vpiLeftRange,rangeHdl),&val);
                        check_vpi_error();
                        m_range_left = val.value.integer;

                        vpi_get_value(vpi_handle(vpiRightRange,rangeHdl),&val);
                        check_vpi_error();
                        m_range_right = val.value.integer;
                    } else {
                        LOG_CRITICAL("Unable to get range for indexable object");
                    }
                }
                else {
                    vpi_get_value(vpi_handle(vpiLeftRange,hdl),&val);
                    check_vpi_error();
                    m_range_left = val.value.integer;

                    vpi_get_value(vpi_handle(vpiRightRange,hdl),&val);
                    check_vpi_error();
                    m_range_right = val.value.integer;
                }

                LOG_DEBUG("VPI: Indexable object initialised with range [%d:%d] and length >%d<", m_range_left, m_range_right, m_num_elems);
            }
        }
    }
    LOG_DEBUG("VPI: %s initialised with %d elements", id.name.c_str(), m_num_elems);
    return GpiObjHdl::initialise(id);
}

const char* VpiSignalObjHdl::get_signal_value_binstr(void)
{
    FENTER
    s_vpi_value value_s = {vpiBinStrVal};

    vpi_get_value(GpiObjHdl::get_handle<vpiHandle>(), &value_s);
    check_vpi_error();

    return value_s.value.str;
}

const char* VpiSignalObjHdl::get_signal_value_str(void)
{
    s_vpi_value value_s = {vpiStringVal};

    vpi_get_value(GpiObjHdl::get_handle<vpiHandle>(), &value_s);
    check_vpi_error();

    return value_s.value.str;
}

double VpiSignalObjHdl::get_signal_value_real(void)
{
    FENTER
    s_vpi_value value_s = {vpiRealVal};

    vpi_get_value(GpiObjHdl::get_handle<vpiHandle>(), &value_s);
    check_vpi_error();

    return value_s.value.real;
}

long VpiSignalObjHdl::get_signal_value_long(void)
{
    FENTER
    s_vpi_value value_s = {vpiIntVal};

    vpi_get_value(GpiObjHdl::get_handle<vpiHandle>(), &value_s);
    check_vpi_error();

    return value_s.value.integer;
}

// Value related functions
int VpiSignalObjHdl::set_signal_value(long value)
{
    s_vpi_value value_s;

    value_s.value.integer = value;
    value_s.format = vpiIntVal;

    return set_signal_value(value_s);
}

int VpiSignalObjHdl::set_signal_value(double value)
{
    s_vpi_value value_s;

    value_s.value.real = value;
    value_s.format = vpiRealVal;

    return set_signal_value(value_s);
}

int VpiSignalObjHdl::set_signal_value(std::string &value)
{
    s_vpi_value value_s;

    std::vector<char> writable(value.begin(), value.end());
    writable.push_back('\0');

    value_s.value.str = &writable[0];
    value_s.format = vpiBinStrVal;

    return set_signal_value(value_s);
}

int VpiSignalObjHdl::set_signal_value(s_vpi_value value_s)
{
    FENTER

    s_vpi_time vpi_time_s;

    vpi_time_s.type = vpiSimTime;
    vpi_time_s.high = 0;
    vpi_time_s.low  = 0;

    // Use Inertial delay to schedule an event, thus behaving like a verilog testbench
    vpi_put_value(GpiObjHdl::get_handle<vpiHandle>(), &value_s, &vpi_time_s, vpiInertialDelay);
    check_vpi_error();

    FEXIT
    return 0;
}

GpiCbHdl * VpiSignalObjHdl::value_change_cb(unsigned int edge)
{
    VpiValueCbHdl *cb = NULL;

    switch (edge) {
    case 1:
        cb = &m_rising_cb;
        break;
    case 2:
        cb = &m_falling_cb;
        break;
    case 3:
        cb = &m_either_cb;
        break;
    default:
        return NULL;
    }

    if (cb->arm_callback()) {
        return NULL;
    }

    return cb;
}

VpiValueCbHdl::VpiValueCbHdl(GpiImplInterface *impl,
                             VpiSignalObjHdl *sig,
                             int edge) :GpiCbHdl(impl),
                                        VpiCbHdl(impl),
                                        GpiValueCbHdl(impl,sig,edge)
{
    vpi_time.type = vpiSuppressTime;
    m_vpi_value.format = vpiIntVal;

    cb_data.reason = cbValueChange;
    cb_data.time = &vpi_time;
    cb_data.value = &m_vpi_value;
    cb_data.obj = m_signal->get_handle<vpiHandle>();
}

int VpiValueCbHdl::cleanup_callback(void)
{
    if (m_state == GPI_FREE)
        return 0;

    /* This is a recurring callback so just remove when
     * not wanted */
    if (!(vpi_remove_cb(get_handle<vpiHandle>()))) {
        LOG_CRITICAL("VPI: unbale to remove callback : ABORTING");
    }

    m_obj_hdl = NULL;
    m_state = GPI_FREE;
    return 0;
}

VpiStartupCbHdl::VpiStartupCbHdl(GpiImplInterface *impl) : GpiCbHdl(impl),
                                                           VpiCbHdl(impl)
{
#ifndef IUS
    cb_data.reason = cbStartOfSimulation;
#else
    vpi_time.high = (uint32_t)(0);
    vpi_time.low  = (uint32_t)(0);
    vpi_time.type = vpiSimTime;
    cb_data.reason = cbAfterDelay;
#endif
}

int VpiStartupCbHdl::run_callback(void) {
    s_vpi_vlog_info info;
    gpi_sim_info_t sim_info;

    vpi_get_vlog_info(&info);

    sim_info.argc = info.argc;
    sim_info.argv = info.argv;
    sim_info.product = info.product;
    sim_info.version = info.version;

    gpi_embed_init(&sim_info);

    return 0;
}

VpiShutdownCbHdl::VpiShutdownCbHdl(GpiImplInterface *impl) : GpiCbHdl(impl),
                                                             VpiCbHdl(impl)
{
    cb_data.reason = cbEndOfSimulation;
}

int VpiShutdownCbHdl::run_callback(void) {
    gpi_embed_end();
    return 0;
}

VpiTimedCbHdl::VpiTimedCbHdl(GpiImplInterface *impl, uint64_t time_ps) : GpiCbHdl(impl),
                                                                         VpiCbHdl(impl)
{
    vpi_time.high = (uint32_t)(time_ps>>32);
    vpi_time.low  = (uint32_t)(time_ps);
    vpi_time.type = vpiSimTime;

    cb_data.reason = cbAfterDelay;
}

int VpiTimedCbHdl::cleanup_callback(void)
{
    switch (m_state) {
    case GPI_PRIMED:
        /* Issue #188: Work around for modelsim that is harmless to others too,
           we tag the time as delete, let it fire then do not pass up
           */
        LOG_DEBUG("Not removing PRIMED timer %d\n", vpi_time.low);
        m_state = GPI_DELETE;
        return 0;
    case GPI_DELETE:
        LOG_DEBUG("Removing DELETE timer %d\n", vpi_time.low);
    default:
        break;
    }
    VpiCbHdl::cleanup_callback();
    /* Return one so we delete this object */
    return 1;
}

VpiReadwriteCbHdl::VpiReadwriteCbHdl(GpiImplInterface *impl) : GpiCbHdl(impl),
                                                               VpiCbHdl(impl)
{
    cb_data.reason = cbReadWriteSynch;
    delay_kill = false;
}

VpiReadOnlyCbHdl::VpiReadOnlyCbHdl(GpiImplInterface *impl) : GpiCbHdl(impl),
                                                             VpiCbHdl(impl)
{
    cb_data.reason = cbReadOnlySynch;
}

VpiNextPhaseCbHdl::VpiNextPhaseCbHdl(GpiImplInterface *impl) : GpiCbHdl(impl),
                                                               VpiCbHdl(impl)
{
    cb_data.reason = cbNextSimTime;
}

void vpi_mappings(GpiIteratorMapping<int32_t, int32_t> &map)
{
    /* vpiModule */
    int32_t module_options[] = {
        //vpiModule,            // Aldec SEGV on mixed language
        //vpiModuleArray,       // Aldec SEGV on mixed language
        //vpiIODecl,            // Don't care about these
        vpiNet,
        vpiNetArray,
        vpiReg,
        vpiRegArray,
        vpiMemory,
        vpiIntegerVar,
        vpiRealVar,
        vpiStructVar,
        vpiStructNet,
        //vpiVariables          // Aldec SEGV on plain Verilog
        vpiNamedEvent,
        vpiNamedEventArray,
        vpiParameter,
        //vpiSpecParam,         // Don't care
        //vpiParamAssign,       // Aldec SEGV on mixed language
        //vpiDefParam,          // Don't care
        vpiPrimitive,
        vpiPrimitiveArray,
        //vpiContAssign,        // Don't care
        vpiProcess,             // Don't care
        vpiModPath,
        vpiTchk,
        vpiAttribute,
        vpiPort,
        vpiInternalScope,
        //vpiInterface,         // Aldec SEGV on mixed language
        //vpiInterfaceArray,    // Aldec SEGV on mixed language
        0
    };
    map.add_to_options(vpiModule, &module_options[0]);
    map.add_to_options(vpiGenScope, &module_options[0]);

    int32_t struct_options[] = {
        vpiNet,
#ifndef IUS
        vpiNetArray,
#endif
        vpiReg,
        vpiRegArray,
        vpiMemory,
        vpiParameter,
        vpiPrimitive,
        vpiPrimitiveArray,
        vpiAttribute,
        vpiMember,
        0
    };
    map.add_to_options(vpiStructVar, &struct_options[0]);
    map.add_to_options(vpiStructNet, &struct_options[0]);

    /* vpiNet */
    int32_t net_options[] = {
        //vpiContAssign,        // Driver and load handled separately
        //vpiPrimTerm,
        //vpiPathTerm,
        //vpiTchkTerm,
        //vpiDriver,
        //vpiLocalDriver,
        //vpiLoad,
        //vpiLocalLoad,
        vpiNetBit,
        0
    };
    map.add_to_options(vpiNet, &net_options[0]);

    /* vpiNetArray */
    int32_t netarray_options[] = {
        vpiNet,
        0
    };
    map.add_to_options(vpiNetArray, &netarray_options[0]);

    /* vpiRegArray */
    int32_t regarray_options[] = {
        vpiReg,
        0
    };
    map.add_to_options(vpiRegArray, &regarray_options[0]);

    /* vpiMemory */
    int32_t memory_options[] = {
        vpiMemoryWord,
        0
    };
    map.add_to_options(vpiMemory, &memory_options[0]);

    /* vpiPort */
    int32_t port_options[] = {
        vpiPortBit,
        0
    };
    map.add_to_options(vpiPort, &port_options[0]);

    int32_t gate_options[] = {
        vpiPrimTerm,
        vpiTableEntry,
        vpiUdpDefn,
        0
    };
    map.add_to_options(vpiGate, &gate_options[0]);
}

GpiIteratorMapping<int32_t, int32_t> VpiIterator::iterate_over(vpi_mappings);

VpiIterator::VpiIterator(GpiImplInterface *impl, GpiObjHdl *hdl) : GpiIterator(impl, hdl),
                                                                   m_iterator(NULL)
{
    vpiHandle iterator;
    vpiHandle vpi_hdl = m_parent->get_handle<vpiHandle>();

    int type = vpi_get(vpiType, vpi_hdl);
    if (NULL == (selected = iterate_over.get_options(type))) {
        LOG_WARN("VPI: Implementation does not know how to iterate over %s(%d)",
                  vpi_get_str(vpiType, vpi_hdl), type);
        return;
    }


    for (one2many = selected->begin();
         one2many != selected->end();
         one2many++) {

        /* GPI_GENARRAY are pseudo-regions and all that should be searched for are the sub-regions */
        if (m_parent->get_type() == GPI_GENARRAY && *one2many != vpiInternalScope) {
            LOG_DEBUG("vpi_iterator vpiOneToManyT=%d skipped for GPI_GENARRAY type", *one2many);
            continue;
        }

        iterator = vpi_iterate(*one2many, vpi_hdl);

        if (iterator) {
            break;
        }

        LOG_DEBUG("vpi_iterate type=%d returned NULL", *one2many);
    }

    if (NULL == iterator) {
        LOG_DEBUG("vpi_iterate return NULL for all relationships on %s (%d) type:%s",
                  vpi_get_str(vpiName, vpi_hdl),
                  type,
                  vpi_get_str(vpiType, vpi_hdl));
        selected = NULL;
        return;
    }

    LOG_DEBUG("Created iterator working from type %d %s",
              *one2many,
              vpi_get_str(vpiFullName, vpi_hdl));

    m_iterator = iterator;
}

VpiIterator::~VpiIterator()
{
    if (m_iterator)
        vpi_free_object(m_iterator);
}

#define VPI_TYPE_MAX (1000)

GpiIterator::Status VpiSingleIterator::next_handle(std::string &name,
                                                   GpiObjHdl **hdl,
                                                   void **raw_hdl)
{
    GpiObjHdl *new_obj;
    vpiHandle obj;

    if (NULL == m_iterator)
        return GpiIterator::END;

    obj = vpi_scan(m_iterator);
    if (NULL == obj)
        return GpiIterator::END;

    const char *c_name = vpi_get_str(vpiName, obj);
    if (!c_name) {
        int type = vpi_get(vpiType, obj);

        if (type >= VPI_TYPE_MAX) {
            *raw_hdl = (void*)obj;
            return GpiIterator::NOT_NATIVE_NO_NAME;
        }

        LOG_DEBUG("Unable to get the name for this object of type %d", type);

        return GpiIterator::NATIVE_NO_NAME;
    }

    LOG_DEBUG("vpi_scan found '%s", name.c_str());

    new_obj = m_impl->create_and_initialise_gpi_obj(m_parent, obj, name, false);
    if (new_obj) {
        *hdl = new_obj;
        return GpiIterator::NATIVE;
    }
    else
        return GpiIterator::NOT_NATIVE;
}

GpiIterator::Status VpiIterator::next_handle(std::string &name, GpiObjHdl **hdl, void **raw_hdl)
{
    GpiObjHdl *new_obj;
    vpiHandle obj;
    vpiHandle iter_obj = m_parent->get_handle<vpiHandle>();

    bool pseudo = false;
    bool use_index = false;
    int32_t index;

    if (!selected)
        return GpiIterator::END;

    gpi_objtype_t obj_type  = m_parent->get_type();
    std::string parent_name = m_parent->get_name();

    do {
        obj = NULL;

        if (m_iterator) {
            obj = vpi_scan(m_iterator);

            if (obj != NULL) {
                /* For GPI_GENARRAY, only allow the generate statements through that match the name
                 * of the generate block.
                 */
                if (obj_type == GPI_GENARRAY) {
                    if (vpi_get(vpiType, obj) == vpiGenScope) {
                        std::string rgn_name = vpi_get_str(vpiName, obj);
                        if (rgn_name.compare(0,parent_name.length(),parent_name) != 0) {
                            obj = NULL;
                            continue;
                        }
                    } else {
                        obj = NULL;
                        continue;
                    }
                } else if ((*one2many             == vpiInternalScope) &&
                           (vpi_get(vpiType, obj) == vpiGenScope     )) {
                    std::string rgn_name = vpi_get_str(vpiFullName, obj);
                    std::size_t found    = rgn_name.rfind("[");

                    if (found != std::string::npos && found != 0) {
                        rgn_name = rgn_name.substr(0,found);
                        if (pseudo_region_exists(rgn_name)) {
                            LOG_DEBUG("Skipping - Pseudo-Region already exists for %s (%s)", vpi_get_str(vpiFullName, obj),
                                                                                             vpi_get(vpiType, obj));
                            obj=NULL;
                            continue;
                        }
                    }
                }

                LOG_DEBUG("Found an item %s", vpi_get_str(vpiFullName, obj));
                break;

            } else {
                LOG_DEBUG("vpi_scan on %d returned NULL", *one2many);
            }

            /* m_iterator will already be free'd internally here */
            m_iterator = NULL;

            LOG_DEBUG("End of type=%d iteration", *one2many);
        } else {
            LOG_DEBUG("No valid type=%d iterator", *one2many);
        }

        if (++one2many >= selected->end()) {
            obj = NULL;
            break;
        }

        /* GPI_GENARRAY are pseudo-regions and all that should be searched for are the sub-regions */
        if (obj_type == GPI_GENARRAY && *one2many != vpiInternalScope) {
            LOG_DEBUG("vpi_iterator vpiOneToManyT=%d skipped for GPI_GENARRAY type", *one2many);
            continue;
        }

        m_iterator = vpi_iterate(*one2many, iter_obj);

    } while (!obj);

    if (NULL == obj) {
        LOG_DEBUG("No more children, all relationships tested");
        return GpiIterator::END;
    }

    /* Simulators vary here. Some will allow the name to be accessed
       across boundary. We can simply return this up and allow
       the object to be created. Others do not. In this case
       we see if the object is in out type range and if not
       return the raw_hdl up */

    const char *c_name = vpi_get_str(vpiName, obj);
    if (!c_name) {
        /* This may be another type */
        int type = vpi_get(vpiType, obj);

        if (type >= VPI_TYPE_MAX) {
            *raw_hdl = (void*)obj;
            return GpiIterator::NOT_NATIVE_NO_NAME;
        }

        LOG_DEBUG("Unable to get the name for this object of type %d", type);

        return GpiIterator::NATIVE_NO_NAME;
    }

    /*
     * If the parent is not a generate loop, then watch for generate handles and create
     * the pseudo-region.
     */
    if (obj_type == GPI_GENARRAY || (*one2many == vpiInternalScope && vpi_get(vpiType, obj) == vpiGenScope)) {
        std::string idx_str = c_name;
        std::size_t found = idx_str.rfind("[");

        if (found != std::string::npos && found != 0) {
            if (obj_type != GPI_GENARRAY) {
                name        = idx_str.substr(0,found);
                obj         = m_parent->get_handle<vpiHandle>();

                std::string fullname = vpi_get_str(vpiFullName, obj);
                m_pseudo_regions.push_back(fullname.substr(0, fullname.length()-found));
                pseudo = true;
            } else {
                use_index = true;
                name      = c_name;
                found    += 1;
                index     = strtol(name.substr(found).c_str(), NULL, 10);
            }
        } else {
            name = c_name;
        }
    } else {
        name = c_name;
    }

    /* We try and create a handle internally, if this is not possible we
       return and GPI will try other implementations with the name
       */
    if (use_index)
        new_obj = m_impl->create_and_initialise_gpi_obj(m_parent, obj, index, pseudo);
    else
        new_obj = m_impl->create_and_initialise_gpi_obj(m_parent, obj, name, pseudo);

    if (new_obj) {
        LOG_DEBUG("vpi_scan found '%s'", new_obj->get_fullname_str());
        *hdl = new_obj;
        return GpiIterator::NATIVE;
    }
    else
        return GpiIterator::NOT_NATIVE;
}
