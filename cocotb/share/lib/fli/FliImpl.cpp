/******************************************************************************
* Copyright (c) 2014, 2018 Potential Ventures Ltd
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright
*      notice, this list of conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright
*      notice, this list of conditions and the following disclaimer in the
*      documentation and/or other materials provided with the distribution.
*    * Neither the name of Potential Ventures Ltd
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

#include <cstddef>
#include <string>
#include <vector>

#include "FliImpl.h"
#include "mti.h"
#include "acc_vhdl.h"   // Messy :(
#include "acc_user.h"

extern "C" {
static FliProcessCbHdl *sim_init_cb;
static FliProcessCbHdl *sim_finish_cb;
static FliImpl         *fli_table;

bool fli_is_logic(mtiTypeIdT type)
{
    mtiInt32T numEnums = mti_TickLength(type);
    if (numEnums == 2) {
        char **enum_values = mti_GetEnumValues(type);
        std::string str0 = enum_values[0];
        std::string str1  = enum_values[1];

        if (str0.compare("'0'") == 0 && str1.compare("'1'") == 0) {
            return true;
        }
    } else if (numEnums == 9) {
        const char enums[9][4] = {"'U'","'X'","'0'","'1'","'Z'","'W'","'L'","'H'","'-'"};
        char **enum_values = mti_GetEnumValues(type);

        for (int i = 0; i < 9; i++) {
            std::string str = enum_values[i];
            if (str.compare(enums[i]) != 0) {
                return false;
            }
        }

        return true;
    }

    return false;
}

bool fli_is_char(mtiTypeIdT type)
{
    const int NUM_ENUMS_IN_CHAR_TYPE = 256;
    return (mti_TickLength(type) == NUM_ENUMS_IN_CHAR_TYPE);
}

bool fli_is_boolean(mtiTypeIdT type)
{
    if (mti_TickLength(type) == 2) {
        char **enum_values = mti_GetEnumValues(type);
        std::string strFalse = enum_values[0];
        std::string strTrue  = enum_values[1];

        if (strFalse.compare("FALSE") == 0 && strTrue.compare("TRUE") == 0) {
            return true;
        }
    }

    return false;
}

bool fli_is_signal(void *hdl)
{
    return (acc_fetch_type(hdl) == accSignal || acc_fetch_fulltype(hdl) == accAliasSignal);
}

bool fli_is_variable(void *hdl)
{
    mtiTypeIdT _typeid = mti_GetVarType(static_cast<mtiVariableIdT>(hdl));
    PLI_INT32  _type   = mti_GetVarKind(static_cast<mtiVariableIdT>(hdl));

    return ((mti_GetTypeKind(_typeid) == acc_fetch_type(hdl)) && (_type == accAliasConstant
                                                                     || _type == accAliasGeneric
                                                                     || _type == accVHDLConstant
                                                                     || _type == accGeneric
                                                                     || _type == accVariable));
}

bool fli_is_region(void *hdl)
{
    return (!fli_is_signal(hdl) && !fli_is_variable(hdl) && VS_TYPE_IS_VHDL(acc_fetch_fulltype(hdl)));
}

PLI_INT32 fli_handle_fulltype(void *hdl)
{
    if (fli_is_variable(hdl))
        return mti_GetVarKind(static_cast<mtiVariableIdT>(hdl));
    else
        return acc_fetch_fulltype(hdl);
}

bool fli_is_const(void *hdl)
{
    PLI_INT32 _type = fli_handle_fulltype(hdl);
    return (_type == accGeneric || _type == accVHDLConstant
                                || _type == accAliasConstant
                                || _type == accAliasGeneric);
}

} //extern "C"

void FliImpl::sim_end(void)
{
    if (GPI_DELETE != sim_finish_cb->get_call_state()) {
        sim_finish_cb->set_call_state(GPI_DELETE);
        if (mti_NowUpper() == 0 && mti_Now() == 0 && mti_Delta() == 0) {
            mti_Quit();
        } else {
            mti_Break();
        }
    }
}

gpi_objtype_t FliImpl::get_gpi_obj_type(mtiTypeIdT _typeid)
{
    gpi_objtype_t rv;

    switch (mti_GetTypeKind(_typeid)) {
        case MTI_TYPE_ENUM:
            if (fli_is_logic(_typeid))
                rv = GPI_REGISTER;
            else if (fli_is_boolean(_typeid) || fli_is_char(_typeid))
                rv = GPI_INTEGER;
            else
                rv = GPI_ENUM;
            break;
        case MTI_TYPE_SCALAR:
        case MTI_TYPE_PHYSICAL:
            rv = GPI_INTEGER;
            break;
        case MTI_TYPE_REAL:
            rv = GPI_REAL;
            break;
        case MTI_TYPE_ARRAY: {
                mtiTypeIdT   elemType     = mti_GetArrayElementType(_typeid);
                mtiTypeKindT elemTypeKind = mti_GetTypeKind(elemType);

                switch (elemTypeKind) {
                    case MTI_TYPE_ENUM:
                        if (fli_is_logic(elemType))
                            rv = GPI_REGISTER;
                        else if (fli_is_char(elemType))
                            rv = GPI_STRING;
                        else
                            rv = GPI_ARRAY;
                        break;
                    default:
                        rv = GPI_ARRAY;
                }
            }
            break;
        case MTI_TYPE_RECORD:
            rv = GPI_STRUCTURE;
            break;
        default:
            rv = GPI_UNKNOWN;
    }

    return rv;
}

GpiObjHdl *FliImpl::create_gpi_obj(GpiObjHdl *parent, void *hdl)
{
    GpiObjHdl *new_obj = NULL;

    FliHdl *fli_hdl = reinterpret_cast<FliHdl *>(hdl);

    if (fli_hdl == NULL) {
        LOG_ERROR("FLI::Tried to create GPI object from handle that was not of type FliHdl");
        return NULL;
    }

    switch (fli_hdl->tag) {
        case FliHdl::REGION:
            new_obj = create_gpi_obj(parent, fli_hdl->r);
            break;
        case FliHdl::SIGNAL:
            new_obj = create_gpi_obj(parent, fli_hdl->s);
            break;
        case FliHdl::VARIABLE:
            new_obj = create_gpi_obj(parent, fli_hdl->v);
            break;
        default:
            break;
    }

    return new_obj;
}

GpiObjHdl *FliImpl::create_gpi_obj(GpiObjHdl *parent, mtiRegionIdT hdl)
{
    GpiObjHdl *new_obj = NULL;

    if (!VS_TYPE_IS_VHDL(acc_fetch_fulltype(hdl))) {
        LOG_DEBUG("Handle is not a VHDL type.");
        return NULL;
    }

    new_obj = new FliObjHdl(this, parent, static_cast<mtiRegionIdT>(hdl), GPI_MODULE);

    return new_obj;
}

GpiObjHdl *FliImpl::create_gpi_obj(GpiObjHdl *parent, mtiSignalIdT hdl)
{
    GpiObjHdl *new_obj = NULL;

    switch (get_gpi_obj_type(mti_GetSignalType(hdl))) {
        case GPI_ENUM:
            new_obj = new FliEnumObjHdl(this, parent, hdl);
            break;
        case GPI_REGISTER:
            new_obj = new FliLogicObjHdl(this, parent, hdl);
            break;
        case GPI_INTEGER:
            new_obj = new FliIntObjHdl(this, parent, hdl);
            break;
        case GPI_REAL:
            new_obj = new FliRealObjHdl(this, parent, hdl);
            break;
        case GPI_STRING:
            new_obj = new FliStringObjHdl(this, parent, hdl);
            break;
        case GPI_ARRAY:
            new_obj = new FliArrayObjHdl(this, parent, hdl);
            break;
        case GPI_STRUCTURE:
            new_obj = new FliRecordObjHdl(this, parent, hdl);
            break;
        default:
            return NULL;
    }

    return new_obj;
}

GpiObjHdl *FliImpl::create_gpi_obj(GpiObjHdl *parent, mtiVariableIdT hdl)
{
    GpiObjHdl *new_obj = NULL;

    bool is_const = fli_is_const(hdl);

    switch (get_gpi_obj_type(mti_GetVarType(hdl))) {
        case GPI_ENUM:
            new_obj = new FliEnumObjHdl(this, parent, hdl, is_const);
            break;
        case GPI_REGISTER:
            new_obj = new FliLogicObjHdl(this, parent, hdl, is_const);
            break;
        case GPI_INTEGER:
            new_obj = new FliIntObjHdl(this, parent, hdl, is_const);
            break;
        case GPI_REAL:
            new_obj = new FliRealObjHdl(this, parent, hdl, is_const);
            break;
        case GPI_STRING:
            new_obj = new FliStringObjHdl(this, parent, hdl, is_const);
            break;
        case GPI_ARRAY:
            new_obj = new FliArrayObjHdl(this, parent, hdl, is_const);
            break;
        case GPI_STRUCTURE:
            new_obj = new FliRecordObjHdl(this, parent, hdl, is_const);
            break;
        default:
            return NULL;
    }

    return new_obj;
}

GpiObjHdl *FliImpl::create_gpi_pseudo_obj(GpiObjHdl *parent, void *hdl, gpi_objtype_t objtype) {
    GpiObjHdl *new_obj = NULL;

    FliHdl *fli_hdl = reinterpret_cast<FliHdl *>(hdl);

    if (fli_hdl == NULL) {
        LOG_ERROR("FLI::Tried to create GPI object from handle that was not of type FliHdl");
        return NULL;
    }

    if (objtype == GPI_GENARRAY)
        new_obj = new FliPseudoGenArrayObjHdl(this, parent, fli_hdl->r);
    else if (objtype == GPI_ARRAY)
        LOG_ERROR("FLI::Pseudo-Handle for GPI_ARRAY should never occur");

    return new_obj;
}

GpiObjHdl* FliImpl::native_check_create(void *raw_hdl, GpiObjHdl *parent)
{
    LOG_DEBUG("Trying to convert a raw handle to an FLI Handle.");

    const char * c_name     = acc_fetch_name(raw_hdl);

    if (!c_name) {
        LOG_DEBUG("Unable to query the name of the raw handle.");
        return NULL;
    }

    std::string name    = c_name;

    FliHdl fli_hdl;

    if (fli_is_region(raw_hdl)) {
        fli_hdl = FliHdl(static_cast<mtiRegionIdT>(raw_hdl));
    } else if (fli_is_signal(raw_hdl)) {
        fli_hdl = FliHdl(static_cast<mtiSignalIdT>(raw_hdl));
    } else if (fli_is_variable(raw_hdl)) {
        fli_hdl = FliHdl(static_cast<mtiVariableIdT>(raw_hdl));
    } else {
        return NULL;
    }

    return create_and_initialise_gpi_obj(parent, &fli_hdl, name, false);
}

/**
 * @name    Native Check Create
 * @brief   Determine whether a simulation object is native to FLI and create
 *          a handle if it is
 */
GpiObjHdl*  FliImpl::native_check_create(std::string &name, GpiObjHdl *parent)
{
    bool search_rgn       = false;
    bool search_sig       = false;
    bool search_var       = false;

    std::string   fq_name  = GpiImplInterface::get_handle_fullname(parent, name);
    gpi_objtype_t obj_type = parent->get_type();

    if (obj_type == GPI_MODULE) {
        search_rgn = true;
        search_sig = true;
        search_var = true;
    } else if (obj_type == GPI_STRUCTURE) {
        search_rgn = false;
        search_var = fli_is_variable(parent->get_handle<void *>());
        search_sig = !search_var;
    } else {
        LOG_ERROR("FLI: Parent of type %d must be of type GPI_MODULE or GPI_STRUCTURE to have a child.", obj_type);
        return NULL;
    }

    LOG_DEBUG("Looking for child %s from %s", name.c_str(), parent->get_name_str());

    std::vector<char> writable(fq_name.begin(), fq_name.end());
    writable.push_back('\0');

    mtiRegionIdT rgn;
    mtiSignalIdT sig;
    mtiVariableIdT var;
    FliHdl fli_hdl;

    if (search_rgn && (rgn = mti_FindRegion(&writable[0])) != NULL) {
        bool pseudo = false;

        /* Generate Loops have inconsistent behavior across fli.  A "name"
         * without an index, i.e. dut.loop vs dut.loop(0), will attempt to map
         * to index 0, if index 0 exists.  If it doesn't then it won't find anything.
         *
         * If this unique case is hit, we need to create the Pseudo-region, with the handle
         * being equivalent to the parent region.
         */
        if (acc_fetch_fulltype(rgn) == accForGenerate) {
            rgn = mti_HigherRegion(rgn);
            pseudo = true;
        }

        LOG_DEBUG("Found region %s -> %p", fq_name.c_str(), rgn);

        fli_hdl = FliHdl(rgn);
        return create_and_initialise_gpi_obj(parent, &fli_hdl, name, pseudo);
    } else if (search_sig && (sig = mti_FindSignal(&writable[0])) != NULL) {
        LOG_DEBUG("Found a signal %s -> %p", fq_name.c_str(), sig);

        fli_hdl = FliHdl(sig);
        return create_and_initialise_gpi_obj(parent, &fli_hdl, name, false);
    } else if (search_var && (var = mti_FindVar(&writable[0])) != NULL) {
        LOG_DEBUG("Found a variable %s -> %p", fq_name.c_str(), var);

        fli_hdl = FliHdl(var);
        return create_and_initialise_gpi_obj(parent, &fli_hdl, name, false);
    } else if (search_rgn){
        /* If not found, check to see if the name of a generate loop and create a pseudo-region */
        for (rgn = mti_FirstLowerRegion(parent->get_handle<mtiRegionIdT>()); rgn != NULL; rgn = mti_NextRegion(rgn)) {
            if (acc_fetch_fulltype(rgn) == accForGenerate) {
                std::string rgn_name = mti_GetRegionName(rgn);
                if (rgn_name.compare(0,name.length(),name) == 0) {
                    fli_hdl = FliHdl(mti_HigherRegion(rgn));
                    return create_and_initialise_gpi_obj(parent, &fli_hdl, name, true);
                }
            }
        }
    }

    LOG_DEBUG("Didn't find anything named %s", &writable[0]);
    return NULL;
}

/**
 * @name    Native Check Create
 * @brief   Determine whether a simulation object is native to FLI and create
 *          a handle if it is
 */
GpiObjHdl*  FliImpl::native_check_create(int32_t index, GpiObjHdl *parent)
{
    gpi_objtype_t obj_type = parent->get_type();

    FliHdl fli_hdl;

    LOG_DEBUG("Looking for index %d from %s", index, parent->get_name_str());

    if (obj_type == GPI_GENARRAY) {
        mtiRegionIdT rgn;

        std::string fq_name = GpiImplInterface::get_handle_fullname(parent, index);

        std::vector<char> writable(fq_name.begin(), fq_name.end());
        writable.push_back('\0');

        if ((rgn = mti_FindRegion(&writable[0])) != NULL) {
            LOG_DEBUG("Found region %s -> %p", fq_name.c_str(), rgn);
        } else {
            LOG_DEBUG("Didn't find anything named %s", &writable[0]);
            return NULL;
        }

        fli_hdl = FliHdl(rgn);
        return create_and_initialise_gpi_obj(parent, &fli_hdl, index, false);
    } else if (obj_type == GPI_REGISTER || obj_type == GPI_ARRAY || obj_type == GPI_STRING) {
        if (!parent->get_indexable()) {
            LOG_DEBUG("Handle is not indexable");
            return NULL;
        }

        int left  = parent->get_range_left();
        int right = parent->get_range_right();
        int32_t norm_idx;

        if (left > right) {
            norm_idx = left - index;
        } else {
            norm_idx = index - left;
        }

        if (norm_idx < 0 || norm_idx >= parent->get_num_elems()) {
            LOG_DEBUG("Invalid index: %d is out of range [%d,%d]", index, left, right);
            return NULL;
        }

        void *parent_hdl = parent->get_handle<void *>();

        if (fli_is_variable(parent_hdl)) {
            mtiVariableIdT *handles = mti_GetVarSubelements(static_cast<mtiVariableIdT>(parent_hdl), NULL);
            mtiVariableIdT hdl;

            if (handles == NULL) {
                LOG_DEBUG("Error allocating memory for array elements");
                return NULL;
            }
            hdl = handles[norm_idx];
            mti_VsimFree(handles);

            fli_hdl = FliHdl(hdl);
            return create_and_initialise_gpi_obj(parent, &fli_hdl, index, false);
        } else {
            mtiSignalIdT *handles = mti_GetSignalSubelements(static_cast<mtiSignalIdT>(parent_hdl), NULL);
            mtiSignalIdT hdl;

            if (handles == NULL) {
                LOG_DEBUG("Error allocating memory for array elements");
                return NULL;
            }
            hdl = handles[norm_idx];
            mti_VsimFree(handles);

            fli_hdl = FliHdl(hdl);
            return create_and_initialise_gpi_obj(parent, &fli_hdl, index, false);
        }

    } else {
        LOG_ERROR("FLI: Parent of type %d must be of type GPI_GENARRAY, GPI_REGISTER, GPI_ARRAY, or GPI_STRING to have an index.", obj_type);
        return NULL;
    }
}

size_t FliImpl::get_handle_name_len(GpiObjHdl *hdl, bool full)
{
    size_t len        = 0;
    size_t concat_len = (full) ? 1 : 0;                    // Add one for concat char '/' or '.'
    GpiObjHdl *current = hdl;

    if (current == NULL) {
        return 0;
    }

    do {
        while (current->use_index()) {
            len += current->get_id_index_str().length() + 2;  // Add 2 for the '(' and ')'
            current = current->get_parent();
        }

        len += current->get_id_name().length();
        len += concat_len;

        current = current->get_parent();

    } while (full && current != NULL);

    return len;
}

std::string FliImpl::get_handle_name(GpiObjHdl *hdl)
{
    std::string name;

    GpiObjHdl *current = hdl;
    size_t len = get_handle_name_len(hdl, false);
    size_t idx = len;
    char *buff = new char[len+1];

    buff[idx] = '\0';

    while (current->use_index()) {
        std::string index_str = current->get_id_index_str();

        buff[--idx] = ')';
        idx -= index_str.length();
        index_str.copy(&buff[idx], std::string::npos);
        buff[--idx] = '(';

        current = current->get_parent();
    }

    current->get_id_name().copy(buff, std::string::npos);

    name = buff;

    delete buff;

    return name;
}

std::string FliImpl::get_handle_fullname(GpiObjHdl *hdl)
{
    GpiObjHdl *current = hdl;

    if (current == NULL) {
        return "";
    }

    std::string name;
    size_t len = get_handle_name_len(hdl, true);
    size_t idx = len;
    char *buff = new char[len+1];

    buff[idx] = '\0';

    do {
        while (current->use_index()) {
            std::string index_str = current->get_id_index_str();

            buff[--idx] = ')';
            idx -= index_str.length();
            index_str.copy(&buff[idx], std::string::npos);
            buff[--idx] = '(';

            current = current->get_parent();
        }

        idx -= current->get_id_name().length();
        current->get_id_name().copy(&buff[idx], std::string::npos);

        current = current->get_parent();

        buff[--idx] = ((current != NULL) && (current->get_type() == GPI_STRUCTURE)) ? '.' : '/';
    } while (current != NULL);

    name = buff;

    delete buff;

    return name;
}

const char *FliImpl::reason_to_string(int reason)
{
    return "Who can explain it, who can tell you why?";
}


/**
 * @name    Get current simulation time
 * @brief   Get current simulation time
 *
 * NB units depend on the simulation configuration
 */
void FliImpl::get_sim_time(uint32_t *high, uint32_t *low)
{
    *high = mti_NowUpper();
    *low = mti_Now();
}

void FliImpl::get_sim_precision(int32_t *precision)
{
    *precision = mti_GetResolutionLimit();
}

/**
 * @name    Find the root handle
 * @brief   Find the root handle using an optional name
 *
 * Get a handle to the root simulator object.  This is usually the toplevel.
 *
 * If no name is provided, we return the first root instance.
 *
 * If name is provided, we check the name against the available objects until
 * we find a match.  If no match is found we return NULL
 */
GpiObjHdl *FliImpl::get_root_handle(const char *name)
{
    mtiRegionIdT root;
    std::string root_name;
    FliHdl fli_hdl;

    for (root = mti_GetTopRegion(); root != NULL; root = mti_NextRegion(root)) {
        LOG_DEBUG("Iterating over: %s", mti_GetRegionName(root));
        if (name == NULL || !strcmp(name, mti_GetRegionName(root)))
            break;
    }

    if (!root) {
        goto error;
    }

    root_name = mti_GetRegionName(root);

    LOG_DEBUG("Found toplevel: %s, creating handle....", root_name.c_str());

    fli_hdl = FliHdl(root);
    return create_and_initialise_gpi_obj(NULL, &fli_hdl, root_name, false);

error:

    LOG_ERROR("FLI: Couldn't find root handle %s", name);

    for (root = mti_GetTopRegion(); root != NULL; root = mti_NextRegion(root)) {
        if (name == NULL)
            break;

        LOG_ERROR("FLI: Toplevel instances: %s != %s...", name, mti_GetRegionName(root));
    }
    return NULL;
}


GpiCbHdl *FliImpl::register_timed_callback(uint64_t time_ps)
{
    FliTimedCbHdl *hdl = cache.get_timer(time_ps);

    if (hdl->arm_callback()) {
        delete(hdl);
        hdl = NULL;
    }
    return hdl;
}


GpiCbHdl *FliImpl::register_readonly_callback(void)
{
    if (m_readonly_cbhdl.arm_callback()) {
        return NULL;
    }
    return &m_readonly_cbhdl;
}

GpiCbHdl *FliImpl::register_readwrite_callback(void)
{
    if (m_readwrite_cbhdl.arm_callback()) {
        return NULL;
    }
    return &m_readwrite_cbhdl;
}

GpiCbHdl *FliImpl::register_nexttime_callback(void)
{
    if (m_nexttime_cbhdl.arm_callback()) {
        return NULL;
    }
    return &m_nexttime_cbhdl;
}


int FliImpl::deregister_callback(GpiCbHdl *gpi_hdl)
{
    return gpi_hdl->cleanup_callback();
}


GpiIterator *FliImpl::iterate_handle(GpiObjHdl *obj_hdl, gpi_iterator_sel_t type)
{
    GpiIterator *new_iter = NULL;

    switch (type) {
        case GPI_OBJECTS:
            new_iter = new FliIterator(this, obj_hdl);
            break;
        default:
            LOG_WARN("Other iterator types not implemented yet");
            break;
    }

    return new_iter;
}

void fli_mappings(GpiIteratorMapping<int, FliIterator::OneToMany> &map)
{
    FliIterator::OneToMany region_options[] = {
        FliIterator::OTM_CONSTANTS,
        FliIterator::OTM_SIGNALS,
        FliIterator::OTM_REGIONS,
        FliIterator::OTM_END,
    };
    map.add_to_options(accArchitecture, &region_options[0]);
    map.add_to_options(accEntityVitalLevel0, &region_options[0]);
    map.add_to_options(accArchVitalLevel0, &region_options[0]);
    map.add_to_options(accArchVitalLevel1, &region_options[0]);
    map.add_to_options(accBlock, &region_options[0]);
    map.add_to_options(accCompInst, &region_options[0]);
    map.add_to_options(accDirectInst, &region_options[0]);
    map.add_to_options(accinlinedBlock, &region_options[0]);
    map.add_to_options(accinlinedinnerBlock, &region_options[0]);
    map.add_to_options(accGenerate, &region_options[0]);
    map.add_to_options(accIfGenerate, &region_options[0]);
#ifdef accElsifGenerate
    map.add_to_options(accElsifGenerate, &region_options[0]);
#endif
#ifdef accElseGenerate
    map.add_to_options(accElseGenerate, &region_options[0]);
#endif
#ifdef accCaseGenerate
    map.add_to_options(accCaseGenerate, &region_options[0]);
#endif
#ifdef accCaseOTHERSGenerate
    map.add_to_options(accCaseOTHERSGenerate, &region_options[0]);
#endif
    map.add_to_options(accForGenerate, &region_options[0]);
    map.add_to_options(accConfiguration, &region_options[0]);

    FliIterator::OneToMany signal_options[] = {
        FliIterator::OTM_SIGNAL_SUB_ELEMENTS,
        FliIterator::OTM_END,
    };
    map.add_to_options(accSignal, &signal_options[0]);
    map.add_to_options(accSignalBit, &signal_options[0]);
    map.add_to_options(accSignalSubComposite, &signal_options[0]);
    map.add_to_options(accAliasSignal, &signal_options[0]);

    FliIterator::OneToMany variable_options[] = {
        FliIterator::OTM_VARIABLE_SUB_ELEMENTS,
        FliIterator::OTM_END,
    };
    map.add_to_options(accVariable, &variable_options[0]);
    map.add_to_options(accGeneric, &variable_options[0]);
    map.add_to_options(accGenericConstant, &variable_options[0]);
    map.add_to_options(accAliasConstant, &variable_options[0]);
    map.add_to_options(accAliasGeneric, &variable_options[0]);
    map.add_to_options(accAliasVariable, &variable_options[0]);
    map.add_to_options(accVHDLConstant, &variable_options[0]);
}

GpiIteratorMapping<int, FliIterator::OneToMany> FliIterator::iterate_over(fli_mappings);

FliIterator::FliIterator(GpiImplInterface *impl, GpiObjHdl *hdl) : GpiIterator(impl, hdl),
                                                                   m_vars(),
                                                                   m_sigs(),
                                                                   m_regs(),
                                                                   m_currentHandles(NULL)
{
    int type = fli_handle_fulltype(m_parent->get_handle<void *>());

    LOG_DEBUG("fli_iterator::Create iterator for %s of type %d:%s", m_parent->get_fullname().c_str(), type, acc_fetch_type_str(type));

    if (NULL == (selected = iterate_over.get_options(type))) {
        LOG_WARN("FLI: Implementation does not know how to iterate over %s(%d)",
                 acc_fetch_type_str(type), type);
        return;
    }

    /* Find the first mapping type that yields a valid iterator */
    for (one2many = selected->begin(); one2many != selected->end(); one2many++) {
        /* GPI_GENARRAY are pseudo-regions and all that should be searched for are the sub-regions */
        if (m_parent->get_type() == GPI_GENARRAY && *one2many != FliIterator::OTM_REGIONS) {
            LOG_DEBUG("fli_iterator OneToMany=%d skipped for GPI_GENARRAY type", *one2many);
            continue;
        }

        populate_handle_list(*one2many);

        switch (*one2many) {
            case FliIterator::OTM_CONSTANTS:
            case FliIterator::OTM_VARIABLE_SUB_ELEMENTS:
                m_currentHandles = &m_vars;
                m_iterator = m_vars.begin();
                break;
            case FliIterator::OTM_SIGNALS:
            case FliIterator::OTM_SIGNAL_SUB_ELEMENTS:
                m_currentHandles = &m_sigs;
                m_iterator = m_sigs.begin();
                break;
            case FliIterator::OTM_REGIONS:
                m_currentHandles = &m_regs;
                m_iterator = m_regs.begin();
                break;
            default:
                LOG_WARN("Unhandled OneToMany Type (%d)", *one2many);
        }

        if (m_iterator != m_currentHandles->end())
            break;

        LOG_DEBUG("fli_iterator OneToMany=%d returned NULL", *one2many);
    }

    if (m_iterator == m_currentHandles->end()) {
        LOG_DEBUG("fli_iterator return NULL for all relationships on %s (%d) kind:%s",
                  m_parent->get_name_str(), type, acc_fetch_type_str(type));
        selected = NULL;
        return;
    }

    LOG_DEBUG("Created iterator working from scope %d",
              *one2many);
}

GpiIterator::Status FliIterator::next_handle(std::string &name, GpiObjHdl **hdl, void **raw_hdl)
{
    FliHdl *obj;
    FliHdl fli_hdl;
    GpiObjHdl *new_obj;

    bool pseudo    = false;
    bool use_index = false;
    int32_t index;

    if (!selected)
        return GpiIterator::END;

    gpi_objtype_t obj_type  = m_parent->get_type();
    std::string parent_name = m_parent->get_name();

    /* We want the next object in the current mapping.
     * If the end of mapping is reached then we want to
     * try next one until a new object is found
     */
    do {
        obj = NULL;

        if (m_iterator != m_currentHandles->end()) {
            fli_hdl = *m_iterator++;
            obj = &fli_hdl;

            /* For GPI_GENARRAY, only allow the generate statements through that match the name
             * of the generate block.
             */
            if (obj_type == GPI_GENARRAY) {
                if (fli_hdl.tag == FliHdl::REGION && acc_fetch_fulltype(fli_hdl.r) == accForGenerate) {
                    std::string rgn_name = mti_GetRegionName(fli_hdl.r);
                    if (rgn_name.compare(0,parent_name.length(),parent_name) != 0) {
                        obj = NULL;
                        continue;
                    }
                } else {
                    obj = NULL;
                    continue;
                }
            } else if (*one2many == FliIterator::OTM_REGIONS && acc_fetch_fulltype(fli_hdl.r) == accForGenerate) {
                char *rgn_name_c     = mti_GetRegionFullName(fli_hdl.r);
                std::string rgn_name = rgn_name_c;
                std::size_t found    = rgn_name.rfind("(");

                mti_VsimFree(rgn_name_c);

                if (found != std::string::npos && found != 0) {
                    std::string pseudo_region = rgn_name.substr(0,found);
                    if (pseudo_region_exists(pseudo_region)) {
                        LOG_DEBUG("Skipping - Pseudo-Region already exists for %s", rgn_name.c_str());
                        obj=NULL;
                        continue;
                    }
                }
            }
            break;
        } else {
            LOG_DEBUG("No more valid handles in the current OneToMany=%d iterator", *one2many);
        }

        if (++one2many >= selected->end()) {
            obj = NULL;
            break;
        }

        /* GPI_GENARRAY are pseudo-regions and all that should be searched for are the sub-regions */
        if (obj_type == GPI_GENARRAY && *one2many != FliIterator::OTM_REGIONS) {
            LOG_DEBUG("fli_iterator OneToMany=%d skipped for GPI_GENARRAY type", *one2many);
            continue;
        }

        populate_handle_list(*one2many);

        switch (*one2many) {
            case FliIterator::OTM_CONSTANTS:
            case FliIterator::OTM_VARIABLE_SUB_ELEMENTS:
                m_currentHandles = &m_vars;
                m_iterator = m_vars.begin();
                break;
            case FliIterator::OTM_SIGNALS:
            case FliIterator::OTM_SIGNAL_SUB_ELEMENTS:
                m_currentHandles = &m_sigs;
                m_iterator = m_sigs.begin();
                break;
            case FliIterator::OTM_REGIONS:
                m_currentHandles = &m_regs;
                m_iterator = m_regs.begin();
                break;
            default:
                LOG_WARN("Unhandled OneToMany Type (%d)", *one2many);
        }
    } while (!obj);

    if (NULL == obj) {
        LOG_DEBUG("No more children, all relationships tested");
        return GpiIterator::END;
    }

    char *c_name;
    switch (*one2many) {
        case FliIterator::OTM_CONSTANTS:
        case FliIterator::OTM_VARIABLE_SUB_ELEMENTS:
            c_name = mti_GetVarName(obj->v);
            break;
        case FliIterator::OTM_SIGNALS:
            c_name = mti_GetSignalName(obj->s);
            break;
        case FliIterator::OTM_SIGNAL_SUB_ELEMENTS:
            c_name = mti_GetSignalNameIndirect(obj->s, NULL, 0);
            break;
        case FliIterator::OTM_REGIONS:
            c_name = mti_GetRegionName(obj->r);
            break;
        default:
            LOG_WARN("Unhandled OneToMany Type (%d)", *one2many);
    }

    if (!c_name) {
        if (!VS_TYPE_IS_VHDL(fli_handle_fulltype(obj->get_handle()))) {
            *raw_hdl = obj->get_handle();
            return GpiIterator::NOT_NATIVE_NO_NAME;
        }

        return GpiIterator::NATIVE_NO_NAME;
    }

    /*
     * If the parent is not a generate loop, then watch for generate handles and create
     * the pseudo-region.
     */
    if (obj_type == GPI_GENARRAY || (*one2many == FliIterator::OTM_REGIONS && acc_fetch_fulltype(fli_hdl.r) == accForGenerate)) {
        std::string idx_str = c_name;
        std::size_t found = idx_str.rfind("(");

        if (found != std::string::npos && found != 0) {
            if (obj_type != GPI_GENARRAY) {
                name        = idx_str.substr(0,found);
                fli_hdl     = FliHdl(m_parent->get_handle<mtiRegionIdT>());
                obj         = &fli_hdl;

                char *fullname_c     = mti_GetRegionFullName(fli_hdl.r);
                std::string fullname = fullname_c;

                mti_VsimFree(fullname_c);

                m_pseudo_regions.push_back(fullname.substr(0, fullname.length()-found));
                pseudo = true;
            } else {
                use_index = true;
                name      = c_name;
                found    += 1;
                index     = strtol(name.substr(found).c_str(), NULL, 10);
            }
        } else {
            LOG_WARN("Unhandled Generate Loop Format - %s", name.c_str());
            name = c_name;
        }
    } else {
        name = c_name;
    }

    if (*one2many == FliIterator::OTM_SIGNAL_SUB_ELEMENTS) {
        mti_VsimFree(c_name);
    }

    if (use_index)
        new_obj = m_impl->create_and_initialise_gpi_obj(m_parent, obj, index, pseudo);
    else
        new_obj = m_impl->create_and_initialise_gpi_obj(m_parent, obj, name, pseudo);

    if (new_obj) {
        *hdl = new_obj;
        return GpiIterator::NATIVE;
    } else {
        return GpiIterator::NOT_NATIVE;
    }
}

void FliIterator::populate_handle_list(FliIterator::OneToMany childType)
{
    switch (childType) {
        case FliIterator::OTM_CONSTANTS: {
                mtiRegionIdT parent = m_parent->get_handle<mtiRegionIdT>();
                mtiVariableIdT id;

                for (id = mti_FirstVarByRegion(parent); id; id = mti_NextVar()) {
                    if (id) {
                        m_vars.push_back(FliHdl(id));
                    }
                }
            }
            break;
        case FliIterator::OTM_SIGNALS: {
                mtiRegionIdT parent = m_parent->get_handle<mtiRegionIdT>();
                mtiSignalIdT id;

                for (id = mti_FirstSignal(parent); id; id = mti_NextSignal()) {
                    if (id) {
                        m_sigs.push_back(FliHdl(id));
                    }
                }
            }
            break;
        case FliIterator::OTM_REGIONS: {
                mtiRegionIdT parent = m_parent->get_handle<mtiRegionIdT>();
                mtiRegionIdT id;

                for (id = mti_FirstLowerRegion(parent); id; id = mti_NextRegion(id)) {
                    if (id) {
                        m_regs.push_back(FliHdl(id));
                    }
                }
            }
            break;
        case FliIterator::OTM_SIGNAL_SUB_ELEMENTS:
            if (m_parent->get_type() == GPI_STRUCTURE || m_parent->get_indexable()) {
                mtiSignalIdT parent = m_parent->get_handle<mtiSignalIdT>();

                mtiTypeIdT type = mti_GetSignalType(parent);
                mtiSignalIdT *ids = mti_GetSignalSubelements(parent,NULL);

                for (int i = 0; i < mti_TickLength(type); i++) {
                    m_sigs.push_back(FliHdl(ids[i]));
                }
                mti_VsimFree(ids);
            }
            break;
        case FliIterator::OTM_VARIABLE_SUB_ELEMENTS:
            if (m_parent->get_type() == GPI_STRUCTURE || m_parent->get_indexable()) {
                mtiVariableIdT parent = m_parent->get_handle<mtiVariableIdT>();

                mtiTypeIdT type = mti_GetVarType(parent);
                mtiVariableIdT *ids = mti_GetVarSubelements(parent,NULL);

                for (int i = 0; i < mti_TickLength(type); i++) {
                    m_vars.push_back(FliHdl(ids[i]));
                }

                mti_VsimFree(ids);
            }
            break;
        default:
            LOG_WARN("Unhandled OneToMany Type (%d)", childType);
    }
}


FliTimedCbHdl* FliTimerCache::get_timer(uint64_t time_ps)
{
    FliTimedCbHdl *hdl;

    if (!free_list.empty()) {
        hdl = free_list.front();
        free_list.pop();
        hdl->reset_time(time_ps);
    } else {
        hdl = new FliTimedCbHdl(impl, time_ps);
    }

    return hdl;
}

void FliTimerCache::put_timer(FliTimedCbHdl* hdl)
{
    free_list.push(hdl);
}

extern "C" {

// Main re-entry point for callbacks from simulator
void handle_fli_callback(void *data)
{
    fflush(stderr);

    FliProcessCbHdl *cb_hdl = (FliProcessCbHdl*)data;

    if (!cb_hdl) {
        LOG_CRITICAL("FLI: Callback data corrupted: ABORTING");
    }

    gpi_cb_state_e old_state = cb_hdl->get_call_state();

    if (old_state == GPI_PRIMED) {

        cb_hdl->set_call_state(GPI_CALL);

        cb_hdl->run_callback();
        gpi_cb_state_e new_state = cb_hdl->get_call_state();

        /* We have re-primed in the handler */
        if (new_state != GPI_PRIMED)
            if (cb_hdl->cleanup_callback())
                delete cb_hdl;
    } else {
        /* Issue #188 seems to appear via FLI as well */
        cb_hdl->cleanup_callback();
    }
};

static void register_initial_callback(void)
{
    FENTER
    sim_init_cb = new FliStartupCbHdl(fli_table);
    sim_init_cb->arm_callback();
    FEXIT
}

static void register_final_callback(void)
{
    FENTER
    sim_finish_cb = new FliShutdownCbHdl(fli_table);
    sim_finish_cb->arm_callback();
    FEXIT
}

static void register_embed(void)
{
    fli_table = new FliImpl("FLI");
    gpi_register_impl(fli_table);
    gpi_load_extra_libs();
}


void cocotb_init(void)
{
    LOG_INFO("cocotb_init called\n");
    register_embed();
    register_initial_callback();
    register_final_callback();
}

} // extern "C"

GPI_ENTRY_POINT(fli, register_embed);

