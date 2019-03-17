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
*    * Neither the name of Potential Ventures Ltd not the
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

#include "VhpiImpl.h"
#include <cmath>
#include <algorithm>
#include <stdlib.h>

extern "C" {
static VhpiCbHdl *sim_init_cb;
static VhpiCbHdl *sim_finish_cb;
static VhpiImpl  *vhpi_table;
}

vhpiHandleT vhpi_handle_base(vhpiHandleT hdl)
{
    vhpiHandleT base_hdl = vhpi_handle(vhpiBaseType, hdl);

    if (base_hdl == NULL) {
        vhpiHandleT st_hdl = vhpi_handle(vhpiSubtype, hdl);

        if (st_hdl != NULL) {
            base_hdl = vhpi_handle(vhpiBaseType, st_hdl);
            vhpi_release_handle(st_hdl);
        }
    }

    return base_hdl;
}

int32_t vhpi_handle_dimensions(vhpiHandleT hdl)
{
    int32_t     num_dims = -1;
    vhpiHandleT base_hdl;

    if ((base_hdl = vhpi_handle_base(hdl)) != NULL) {
        num_dims = vhpi_get(vhpiNumDimensionsP, base_hdl);

        vhpi_release_handle(base_hdl);
    }

    return num_dims;
}

#define CASE_STR(_X) \
    case _X: return #_X

const char * VhpiImpl::format_to_string(int format)
{
    switch (format) {
        CASE_STR(vhpiBinStrVal);
        CASE_STR(vhpiOctStrVal);
        CASE_STR(vhpiDecStrVal);
        CASE_STR(vhpiHexStrVal);
        CASE_STR(vhpiEnumVal);
        CASE_STR(vhpiIntVal);
        CASE_STR(vhpiLogicVal);
        CASE_STR(vhpiRealVal);
        CASE_STR(vhpiStrVal);
        CASE_STR(vhpiCharVal);
        CASE_STR(vhpiTimeVal);
        CASE_STR(vhpiPhysVal);
        CASE_STR(vhpiObjTypeVal);
        CASE_STR(vhpiPtrVal);
        CASE_STR(vhpiEnumVecVal);
        CASE_STR(vhpiRawDataVal);

        default: return "unknown";
    }
}

const char *VhpiImpl::reason_to_string(int reason)
{
    switch (reason) {
        CASE_STR(vhpiCbValueChange);
        CASE_STR(vhpiCbStartOfNextCycle);
        CASE_STR(vhpiCbStartOfPostponed);
        CASE_STR(vhpiCbEndOfTimeStep);
        CASE_STR(vhpiCbNextTimeStep);
        CASE_STR(vhpiCbAfterDelay);
        CASE_STR(vhpiCbStartOfSimulation);
        CASE_STR(vhpiCbEndOfSimulation);
        CASE_STR(vhpiCbEndOfProcesses);
        CASE_STR(vhpiCbLastKnownDeltaCycle);

        default: return "unknown";
    }
}

#undef CASE_STR

void VhpiImpl::get_sim_time(uint32_t *high, uint32_t *low)
{
    vhpiTimeT vhpi_time_s;
    vhpi_get_time(&vhpi_time_s, NULL);
    check_vhpi_error();
    *high = vhpi_time_s.high;
    *low = vhpi_time_s.low;
}

void VhpiImpl::get_sim_precision(int32_t *precision)
{
    /* The value returned is in number of femtoseconds */
    vhpiPhysT prec = vhpi_get_phys(vhpiResolutionLimitP, NULL);
    uint64_t femtoseconds = ((uint64_t)prec.high << 32) | prec.low;
    double base = 1e-15 * femtoseconds;
    *precision = (int32_t)log10(base);
}

// Determine whether a VHPI object type is a constant or not
bool is_const(vhpiHandleT hdl)
{
    vhpiHandleT tmp = hdl;

    /* Need to walk the prefix's back to the original handle to get a type
     * that is not vhpiSelectedNameK or vhpiIndexedNameK
     */
    do {
        vhpiIntT vhpitype = vhpi_get(vhpiKindP, tmp);
        if (vhpiConstDeclK == vhpitype || vhpiGenericDeclK == vhpitype)
            return true;
    } while ((tmp = vhpi_handle(vhpiPrefix, tmp)) != NULL);

    return false;
}

bool is_enum_logic(vhpiHandleT hdl) {
    const char *type = vhpi_get_str(vhpiNameP, hdl);

    if (0 == strncmp(type, "BIT"       , sizeof("BIT")-1)        ||
        0 == strncmp(type, "STD_ULOGIC", sizeof("STD_ULOGIC")-1) ||
        0 == strncmp(type, "STD_LOGIC" , sizeof("STD_LOGIC")-1)) {
        return true;
    } else {
        vhpiIntT num_enum = vhpi_get(vhpiNumLiteralsP, hdl);

        if (2 == num_enum) {
            vhpiHandleT it = vhpi_iterator(vhpiEnumLiterals, hdl);
            if (it != NULL) {
                const char *enums_1[2] = { "0",   "1"}; //Aldec does not return the single quotes
                const char *enums_2[2] = {"'0'", "'1'"};
                vhpiHandleT enum_hdl;
                int cnt = 0;

                while ((enum_hdl = vhpi_scan(it)) != NULL) {
                    const char *etype = vhpi_get_str(vhpiStrValP, enum_hdl);
                    if (1 < cnt                                                    ||
                        (0 != strncmp(etype, enums_1[cnt], strlen(enums_1[cnt]))  &&
                         0 != strncmp(etype, enums_2[cnt], strlen(enums_2[cnt])))) {
                        vhpi_release_handle(it);
                        return false;
                    }
                    ++cnt;
                }
                return true;
            }
        } else if (9 == num_enum) {
            vhpiHandleT it = vhpi_iterator(vhpiEnumLiterals, hdl);
            if (it != NULL) {
                const char *enums_1[9] = { "U",   "X",   "0",   "1",   "Z",   "W",   "L",   "H",   "-"}; //Aldec does not return the single quotes
                const char *enums_2[9] = {"'U'", "'X'", "'0'", "'1'", "'Z'", "'W'", "'L'", "'H'", "'-'"};
                vhpiHandleT enum_hdl;
                int cnt = 0;

                while ((enum_hdl = vhpi_scan(it)) != NULL) {
                    const char *etype = vhpi_get_str(vhpiStrValP, enum_hdl);
                    if (8 < cnt                                                    ||
                        (0 != strncmp(etype, enums_1[cnt], strlen(enums_1[cnt]))  &&
                         0 != strncmp(etype, enums_2[cnt], strlen(enums_2[cnt])))) {
                        vhpi_release_handle(it);
                        return false;
                    }
                    ++cnt;
                }
                return true;
            }
        }
    }

    return false;
}

bool is_enum_char(vhpiHandleT hdl) {
    const vhpiIntT NUM_ENUMS_IN_CHAR_TYPE = 256;

    const char *type = vhpi_get_str(vhpiNameP, hdl);

    if (0 == strncmp(type, "CHARACTER", sizeof("STD_ULOGIC")-1)) {
        return true;
    } else {
        return (vhpi_get(vhpiNumLiteralsP, hdl) == NUM_ENUMS_IN_CHAR_TYPE);
    }
}

bool is_enum_boolean(vhpiHandleT hdl) {
    const char *type = vhpi_get_str(vhpiNameP, hdl);

    if (0 == strncmp(type, "BOOLEAN", sizeof("BOOLEAN")-1)) {
        return true;
    } else {
        vhpiIntT num_enum = vhpi_get(vhpiNumLiteralsP, hdl);

        if (2 == num_enum) {
            vhpiHandleT it = vhpi_iterator(vhpiEnumLiterals, hdl);
            if (it != NULL) {
                vhpiHandleT enum_hdl;
                int cnt = 0;

                while ((enum_hdl = vhpi_scan(it)) != NULL) {
                    const char *etype = vhpi_get_str(vhpiStrValP, enum_hdl);
                    if (((0 == cnt && 0 != strncmp(etype, "FALSE", strlen("FALSE")))  &&
                         (0 == cnt && 0 != strncmp(etype, "false", strlen("false")))) ||
                        ((1 == cnt && 0 != strncmp(etype, "TRUE" , strlen("TRUE")))   &&
                         (1 == cnt && 0 != strncmp(etype, "true" , strlen("true"))))  ||
                        2 <= cnt) {
                        vhpi_release_handle(it);
                        return false;
                    }
                    ++cnt;
                }
                return true;
            }
        }
    }

    return false;
}

GpiObjHdl *VhpiImpl::create_gpi_obj(GpiObjHdl *parent, void *hdl)
{
    vhpiIntT type;
    vpiHandle new_hdl = static_cast<vhpiHandleT>(hdl);
    gpi_objtype_t gpi_type;
    GpiObjHdl *new_obj = NULL;

    if (vhpiVerilog == (type = vhpi_get(vhpiKindP, new_hdl))) {
        LOG_DEBUG("vhpiVerilog returned from vhpi_get(vhpiType, ...)")
        return NULL;
    }

    std::string name = vhpi_get_str(vhpiCaseNameP, new_hdl);

    /* We need to delve further here to determine how to later set
       the values of an object */
    vhpiHandleT base_hdl = vhpi_handle(vhpiBaseType, new_hdl);

    if (base_hdl == NULL) {
        vhpiHandleT st_hdl = vhpi_handle(vhpiSubtype, new_hdl);

        if (st_hdl != NULL) {
            base_hdl = vhpi_handle(vhpiBaseType, st_hdl);
            vhpi_release_handle(st_hdl);
        }
    }

    vhpiHandleT query_hdl = (base_hdl != NULL) ? base_hdl : new_hdl;

    vhpiIntT base_type = vhpi_get(vhpiKindP, query_hdl);
    vhpiIntT is_static = vhpi_get(vhpiStaticnessP, query_hdl);

    /* Non locally static objects are not accessible for read/write
       so we create this as a GpiObjType
    */
    if (is_static == vhpiGloballyStatic) {
        gpi_type   = GPI_MODULE;
        goto create;
    }

    switch (base_type) {
        case vhpiArrayTypeDeclK: {
            vhpiIntT num_dim = vhpi_get(vhpiNumDimensionsP, query_hdl);

            if (num_dim > 1) {
                LOG_DEBUG("Detected a MULTI-DIMENSIONAL ARRAY type %s", name.c_str());
                gpi_type   = GPI_ARRAY;
            } else {
                vhpiHandleT elem_base_type_hdl = NULL;
                vhpiIntT elem_base_type        = 0;

                /* vhpiElemSubtype is deprecated.  Should be using vhpiElemType, but not supported in all simulators. */
                vhpiHandleT elem_sub_type_hdl  = vhpi_handle(vhpiElemSubtype, query_hdl);

                if (elem_sub_type_hdl != NULL) {
                    elem_base_type_hdl = vhpi_handle(vhpiBaseType, elem_sub_type_hdl);
                    vhpi_release_handle(elem_sub_type_hdl);
                }

                if (elem_base_type_hdl != NULL) {
                    elem_base_type    = vhpi_get(vhpiKindP, elem_base_type_hdl);
                    if (elem_base_type == vhpiEnumTypeDeclK) {
                        if (is_enum_logic(elem_base_type_hdl)) {
                            LOG_DEBUG("Detected a LOGIC VECTOR type %s", name.c_str());
                            gpi_type   = GPI_REGISTER;
                        } else if (is_enum_char(elem_base_type_hdl)) {
                            LOG_DEBUG("Detected a STRING type %s", name.c_str());
                            gpi_type   = GPI_STRING;
                        } else {
                            LOG_DEBUG("Detected a NON-LOGIC ENUM VECTOR type %s", name.c_str());
                            gpi_type   = GPI_ARRAY;
                        }
                    } else {
                        LOG_DEBUG("Detected a NON-ENUM VECTOR type %s", name.c_str());
                        gpi_type   = GPI_ARRAY;
                    }
                } else {
                    LOG_ERROR("Unable to determine the Array Element Base Type for %s.  Defaulting to GPI_ARRAY.", vhpi_get_str(vhpiFullCaseNameP, new_hdl));
                    gpi_type   = GPI_ARRAY;
                }
            }
            break;
        }

        case vhpiEnumTypeDeclK: {
            if (is_enum_logic(query_hdl)) {
                LOG_DEBUG("Detected a LOGIC type %s", name.c_str());
                gpi_type   = GPI_REGISTER;
            } else if (is_enum_char(query_hdl)) {
                LOG_DEBUG("Detected a CHAR type %s", name.c_str());
                gpi_type   = GPI_INTEGER;
            } else if (is_enum_boolean(query_hdl)) {
                LOG_DEBUG("Detected a BOOLEAN/INTEGER type %s", name.c_str());
                gpi_type   = GPI_INTEGER;
            } else {
                LOG_DEBUG("Detected an ENUM type %s", name.c_str());
                gpi_type   = GPI_ENUM;
            }
            break;
        }

        case vhpiIntTypeDeclK: {
            LOG_DEBUG("Detected an INT type %s", name.c_str());
            gpi_type = GPI_INTEGER;
            break;
        }

        case vhpiFloatTypeDeclK: {
            LOG_DEBUG("Detected a REAL type %s", name.c_str());
            gpi_type = GPI_REAL;
            break;
        }

        case vhpiRecordTypeDeclK: {
            LOG_DEBUG("Detected a STRUCTURE type %s", name.c_str());
            gpi_type   = GPI_STRUCTURE;
            break;
        }

        case vhpiProcessStmtK:
        case vhpiSimpleSigAssignStmtK:
        case vhpiCondSigAssignStmtK:
        case vhpiSelectSigAssignStmtK: {
            gpi_type   = GPI_MODULE;
            break;
        }

        case vhpiRootInstK:
        case vhpiIfGenerateK:
        case vhpiForGenerateK:
        case vhpiCompInstStmtK: {
            gpi_type = GPI_MODULE;
            break;
        }

        default: {
            LOG_ERROR("Not able to map type (%s) %u to object",
                      vhpi_get_str(vhpiKindStrP, query_hdl), type);
            new_obj = NULL;
            goto out;
        }
    }

create:
    LOG_DEBUG("Creating %s of type %d (%s)",
              vhpi_get_str(vhpiFullCaseNameP, new_hdl),
              gpi_type,
              vhpi_get_str(vhpiKindStrP, query_hdl));

    if (gpi_type != GPI_ARRAY && gpi_type != GPI_GENARRAY && gpi_type != GPI_MODULE && gpi_type != GPI_STRUCTURE) {
        if (gpi_type == GPI_REGISTER)
            new_obj = new VhpiLogicSignalObjHdl(this, parent, new_hdl, gpi_type, is_const(new_hdl));
        else
            new_obj = new VhpiSignalObjHdl(this, parent, new_hdl, gpi_type, is_const(new_hdl));
    } else if (gpi_type == GPI_ARRAY) {
        new_obj = new VhpiArrayObjHdl(this, parent, new_hdl, gpi_type);
    } else {
        new_obj = new VhpiObjHdl(this, parent, new_hdl, gpi_type);
    }

out:
    if (base_hdl != NULL)
        vhpi_release_handle(base_hdl);

    return new_obj;
}

GpiObjHdl *VhpiImpl::create_gpi_pseudo_obj(GpiObjHdl *parent, void *hdl, gpi_objtype_t objtype) {
    GpiObjHdl *new_obj = NULL;

    if (objtype == GPI_GENARRAY)
        new_obj = new VhpiPseudoGenArrayObjHdl(this, parent, hdl);
    else if (objtype == GPI_ARRAY)
        new_obj = new VhpiPseudoArrayObjHdl(this, parent, hdl);

    return new_obj;
}

GpiObjHdl *VhpiImpl::native_check_create(void *raw_hdl, GpiObjHdl *parent)
{
    LOG_DEBUG("Trying to convert raw to VHPI handle");

    vhpiHandleT new_hdl = (vhpiHandleT)raw_hdl;

    const char *c_name = vhpi_get_str(vhpiCaseNameP, new_hdl);
    if (!c_name) {
        LOG_DEBUG("Unable to query name of passed in handle");
        return NULL;
    }

    std::string name = c_name;

    GpiObjHdl* new_obj = create_and_initialise_gpi_obj(parent, new_hdl, name, false);
    if (new_obj == NULL) {
        vhpi_release_handle(new_hdl);
        LOG_DEBUG("Unable to fetch object %s", c_name);
        return NULL;
    }

    return new_obj;
}

GpiObjHdl *VhpiImpl::native_check_create(std::string &name, GpiObjHdl *parent)
{
    vhpiHandleT vhpi_hdl  = parent->get_handle<vhpiHandleT>();

    vhpiHandleT new_hdl;
    bool pseudo = false;

    std::string fq_name  = GpiImplInterface::get_handle_fullname(parent, name);

    std::vector<char> writable(fq_name.begin(), fq_name.end());
    writable.push_back('\0');

    new_hdl = vhpi_handle_by_name(&writable[0], NULL);

    if (new_hdl == NULL && parent->get_type() == GPI_STRUCTURE) {
        /* vhpi_handle_by_name() doesn't always work for records, specificaly records in generics */
        vhpiHandleT iter = vhpi_iterator(vhpiSelectedNames, vhpi_hdl);
        if (iter != NULL) {
            while ((new_hdl = vhpi_scan(iter)) != NULL) {
                std::string selected_name = vhpi_get_str(vhpiCaseNameP, new_hdl);
                std::size_t found = selected_name.find_last_of(".");

                if (found != std::string::npos) {
                    selected_name = selected_name.substr(found+1);
                }

                if (selected_name == name) {
                    vhpi_release_handle(iter);
                    break;
                }
            }
        }
    } else if (new_hdl == NULL) {
        /* If not found, check to see if the name of a generate loop */
        vhpiHandleT iter = vhpi_iterator(vhpiInternalRegions, vhpi_hdl);

        if (iter != NULL) {
            vhpiHandleT rgn;
            for (rgn = vhpi_scan(iter); rgn != NULL; rgn = vhpi_scan(iter)) {
                if (vhpi_get(vhpiKindP, rgn) == vhpiForGenerateK) {
                    std::string rgn_name = vhpi_get_str(vhpiCaseNameP, rgn);
                    if (rgn_name.compare(0,name.length(),name) == 0) {
                        new_hdl = vhpi_hdl;
                        vhpi_release_handle(iter);
                        pseudo = true;
                        break;
                    }
                }
            }
        }
        if (new_hdl == NULL) {
            LOG_DEBUG("Unable to query vhpi_handle_by_name %s", fq_name.c_str());
            return NULL;
        }
    }

    /* Generate Loops have inconsistent behavior across vhpi.  A "name"
     * without an index, i.e. dut.loop vs dut.loop(0), may or may not map to
     * to the start index.  If it doesn't then it won't find anything.
     *
     * If this unique case is hit, we need to create the Pseudo-region, with the handle
     * being equivalent to the parent handle.
     */
    if (vhpi_get(vhpiKindP, new_hdl) == vhpiForGenerateK) {
        vhpi_release_handle(new_hdl);

        new_hdl = vhpi_hdl;
        pseudo  = true;
    }

    GpiObjHdl* new_obj = create_and_initialise_gpi_obj(parent, new_hdl, name, pseudo);
    if (new_obj == NULL) {
        vhpi_release_handle(new_hdl);
        LOG_DEBUG("Unable to fetch object %s", fq_name.c_str());
        return NULL;
    }

    return new_obj;
}

GpiObjHdl *VhpiImpl::native_check_create(int32_t index, GpiObjHdl *parent)
{
    vhpiHandleT vhpi_hdl = parent->get_handle<vhpiHandleT>();
    vhpiHandleT new_hdl  = NULL;
    bool pseudo          = false;

    gpi_objtype_t obj_type = parent->get_type();

    if (obj_type == GPI_GENARRAY) {
        LOG_DEBUG("Native check create for index %d of parent %s (pseudo-region)",
                  index,
                  parent->get_name_str());

        std::string fq_name = GpiImplInterface::get_handle_fullname(parent, index);

        std::vector<char> writable(fq_name.begin(), fq_name.end());
        writable.push_back('\0');

        new_hdl = vhpi_handle_by_name(&writable[0], NULL);
    } else if (obj_type == GPI_REGISTER || obj_type == GPI_ARRAY || obj_type == GPI_STRING) {
        LOG_DEBUG("Native check create for index %d of parent %s (%s)",
                  index,
                  parent->get_fullname_str(),
                  vhpi_get_str(vhpiKindStrP, vhpi_hdl));

        if (!parent->get_indexable()) {
            LOG_DEBUG("Handle is not indexable");
            return NULL;
        }

        int32_t  ndim = vhpi_handle_dimensions(vhpi_hdl);

        if (ndim < 0) {
            LOG_ERROR("Unable to get the number dimensions for %s", parent->get_fullname_str());
            return NULL;
        }

        uint32_t idx;
        uint32_t scale;

        if (!parent->is_ascending()) {
            idx = parent->get_range_left() - index;
        } else {
            idx = index - parent->get_range_left();
        }

        scale = parent->get_num_elems();

        /* Need to translate the index into a zero-based flattened array index */
        if (ndim > 1) {
            GpiObjHdl *current = parent;

            while (current->is_pseudo()) {
                --ndim;
                current = current->get_parent();

                if (!current->is_ascending()) {
                    idx += (scale * (current->get_range_left() - current->get_id_index()));
                } else {
                    idx += (scale * (current->get_id_index() - current->get_range_left()));
                }

                scale = scale * current->get_num_elems();
            }

            /* If all dimensions are present, ndim will equal 1 since the index of
             * this call has not been counted.
             */
            if (ndim > 1) {
                new_hdl = vhpi_hdl;  // Set to the parent handle to create the pseudo-handle
                pseudo  = true;
            }
        }

        if (new_hdl == NULL) {
            new_hdl = vhpi_handle_by_index(vhpiIndexedNames, vhpi_hdl, idx);
            if (!new_hdl) {
                /* Support for the above seems poor, so if it did not work
                   try an iteration instead, spotty support for multi-dimensional arrays */

                vhpiHandleT iter = vhpi_iterator(vhpiIndexedNames, vhpi_hdl);
                if (iter != NULL) {
                    uint32_t curr_index = 0;
                    while ((new_hdl = vhpi_scan(iter)) != NULL) {
                        if (idx == curr_index) {
                            vhpi_release_handle(iter);
                            break;
                        }
                        curr_index++;
                    }
                }
            }

            if (new_hdl != NULL) {
                LOG_DEBUG("Index (%d->%d) found %s (%s)", index, idx, vhpi_get_str(vhpiCaseNameP, new_hdl), vhpi_get_str(vhpiKindStrP, new_hdl));
            }
        }
    } else {
        LOG_ERROR("VHPI: Parent of type %s must be of type GPI_GENARRAY, GPI_REGISTER, GPI_ARRAY, or GPI_STRING to have an index.", parent->get_type_str());
        return NULL;
    }


    if (new_hdl == NULL) {
        LOG_DEBUG("Unable to query vhpi_handle_by_index %d", index);
        return NULL;
    }

    GpiObjHdl* new_obj = create_and_initialise_gpi_obj(parent, new_hdl, index, pseudo);
    if (new_obj == NULL) {
        vhpi_release_handle(new_hdl);
        LOG_DEBUG("Could not fetch object below entity (%s) at index (%d)",
                  parent->get_name_str(), index);
        return NULL;
    }

    return new_obj;
}

size_t VhpiImpl::get_handle_full_index_str_len(GpiObjHdl *hdl)
{
    size_t len = 0;

    if (hdl == NULL || !hdl->use_index())
        return len;

    if (hdl->get_parent()->get_type() == GPI_GENARRAY) {
        len += std::string(GEN_IDX_SEP_LHS).length();
        len += hdl->get_id_index_str().length();
        len += std::string(GEN_IDX_SEP_RHS).length();
    } else {
        GpiObjHdl *current = hdl;

        len += 1;                                           // ')'
        while (current->use_index()) {
            len += current->get_id_index_str().length();    // index
            len += 1;                                       // '(' or ','
            current = current->get_parent();
            if (current->use_index() && !current->is_pseudo())
                len += 1;                                   // ')'
        }
    }

    return len;
}

size_t VhpiImpl::get_handle_name_len(GpiObjHdl *hdl, bool full)
{
    size_t len        = 0;
    size_t concat_len = (full) ? 1 : 0;                    // Add one for concat char ':' or '.'
    GpiObjHdl *current = hdl;

    if (current == NULL) {
        return 0;
    }

    do {
        if (current->use_index()) {
            len += get_handle_full_index_str_len(current);
            while (current->use_index()) {
                current = current->get_parent();
            }
        }

        len += current->get_id_name().length();
        len += concat_len;

        current = current->get_parent();

    } while (full && current != NULL);

    return len;
}

void VhpiImpl::insert_handle_full_index_str(GpiObjHdl *hdl, char *buff, size_t len)
{
    size_t idx = len;

    if (hdl == NULL || !hdl->use_index())
        return;

    if (hdl->get_parent()->get_type() == GPI_GENARRAY) {
        std::string index = GEN_IDX_SEP_LHS + hdl->get_id_index_str() + GEN_IDX_SEP_RHS;
        idx -= index.length();
        index.copy(&buff[idx], std::string::npos);
    } else {
        GpiObjHdl *current = hdl;

        buff[--idx] = ')';
        while (current->use_index()) {
            idx -= current->get_id_index_str().length();
            current->get_id_index_str().copy(&buff[idx], std::string::npos);

            current = current->get_parent();

            if (current->is_pseudo())
                buff[--idx] = ',';
            else
                buff[--idx] = '(';

            if (current->use_index() && !current->is_pseudo())
                buff[--idx] = ')';
        }
    }
}

std::string VhpiImpl::get_handle_name(GpiObjHdl *hdl)
{
    GpiObjHdl *current = hdl;

    if (hdl == NULL)
        return "";

    std::string name;

    size_t len = get_handle_name_len(hdl, false);
    size_t idx = len;
    char *buff = new char[len+1];

    buff[idx] = '\0';

    if (current->use_index()) {
        size_t index_len = get_handle_full_index_str_len(current);
        idx -= index_len;
        insert_handle_full_index_str(current, &buff[idx], index_len);
        while (current->use_index()) {
            current = current->get_parent();
        }
    }

    idx -= current->get_id_name().length();
    current->get_id_name().copy(&buff[idx], std::string::npos);

    name = buff;
    delete [] buff;

    return name;
}

std::string VhpiImpl::get_handle_fullname(GpiObjHdl *hdl)
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
        if (current->use_index()) {
            size_t index_len = get_handle_full_index_str_len(current);
            idx -= index_len;
            insert_handle_full_index_str(current, &buff[idx], index_len);
            while (current->use_index()) {
                current = current->get_parent();
            }
        }

        idx -= current->get_id_name().length();
        current->get_id_name().copy(&buff[idx], std::string::npos);

        current = current->get_parent();

        buff[--idx] = ((current != NULL) && (current->get_type() == GPI_STRUCTURE)) ? '.' : ':';
    } while (current != NULL);

    name = buff;

    delete [] buff;

    return name;
}

GpiObjHdl *VhpiImpl::get_root_handle(const char* name)
{
    vhpiHandleT root = NULL;
    vhpiHandleT arch = NULL;
    vhpiHandleT dut = NULL;
    std::string root_name;
    const char *found;

    root = vhpi_handle(vhpiRootInst, NULL);
    check_vhpi_error();

    if (!root) {
        LOG_ERROR("VHPI: Attempting to get the vhpiRootInst failed");
        return NULL;
    } else {
        LOG_DEBUG("VHPI: We have found root='%s'", vhpi_get_str(vhpiCaseNameP, root));
    }

    if (name) {
        if (NULL == (dut = vhpi_handle_by_name(name, NULL))) {
            LOG_DEBUG("VHPI: Unable to query by name");
            check_vhpi_error();
        }
    }

    if (!dut) {
        if (NULL == (arch = vhpi_handle(vhpiDesignUnit, root))) {
            LOG_DEBUG("VHPI: Unable to get vhpiDesignUnit via root");
            check_vhpi_error();
            return NULL;
        }

        if (NULL == (dut = vhpi_handle(vhpiPrimaryUnit, arch))) {
            LOG_DEBUG("VHPI: Unable to get vhpiPrimaryUnit via arch");
            check_vhpi_error();
            return NULL;
        }

        /* if this matches the name then it is what we want, but we
           use the handle two levels up as the dut as do not want an
           object of type vhpiEntityDeclK as the dut */

        found = vhpi_get_str(vhpiCaseNameP, dut);
        dut = root;

    } else {
        found = vhpi_get_str(vhpiCaseNameP, dut);
    }

    if (!dut) {
        LOG_ERROR("VHPI: Attempting to get the DUT handle failed");
        return NULL;
    }

    if (!found) {
        LOG_ERROR("VHPI: Unable to query name for DUT handle");
        return NULL;
    }

    if (name != NULL && strcmp(name, found)) {
        LOG_WARN("VHPI: DUT '%s' doesn't match requested toplevel %s", found, name);
        return NULL;
    }

    root_name = found;

    return create_and_initialise_gpi_obj(NULL, dut, root_name, false);

}

GpiIterator *VhpiImpl::iterate_handle(GpiObjHdl *obj_hdl, gpi_iterator_sel_t type)
{
    GpiIterator *new_iter = NULL;

    switch (type) {
        case GPI_OBJECTS:
            new_iter = new VhpiIterator(this, obj_hdl);
            break;
        default:
            LOG_WARN("Other iterator types not implemented yet");
            break;
    }
    return new_iter;
}

GpiCbHdl *VhpiImpl::register_timed_callback(uint64_t time_ps)
{
    VhpiTimedCbHdl *hdl = new VhpiTimedCbHdl(this, time_ps);

    if (hdl->arm_callback()) {
        delete(hdl);
        hdl = NULL;
    }

    return hdl;
}

GpiCbHdl *VhpiImpl::register_readwrite_callback(void)
{
    if (m_read_write.arm_callback())
        return NULL;

    return &m_read_write;
}

GpiCbHdl *VhpiImpl::register_readonly_callback(void)
{
    if (m_read_only.arm_callback())
        return NULL;

    return &m_read_only;
}

GpiCbHdl *VhpiImpl::register_nexttime_callback(void)
{
    if (m_next_phase.arm_callback())
        return NULL;

    return &m_next_phase;
}

int VhpiImpl::deregister_callback(GpiCbHdl *gpi_hdl)
{
    gpi_hdl->cleanup_callback();
    return 0;
}

void VhpiImpl::sim_end(void)
{
    sim_finish_cb->set_call_state(GPI_DELETE);
    vhpi_control(vhpiFinish);
    check_vhpi_error();
}

extern "C" {

// Main entry point for callbacks from simulator
void handle_vhpi_callback(const vhpiCbDataT *cb_data)
{
    VhpiCbHdl *cb_hdl = (VhpiCbHdl*)cb_data->user_data;

    if (!cb_hdl)
        LOG_CRITICAL("VHPI: Callback data corrupted");

    gpi_cb_state_e old_state = cb_hdl->get_call_state();

    if (old_state == GPI_PRIMED) {

        cb_hdl->set_call_state(GPI_CALL);
        cb_hdl->run_callback();

        gpi_cb_state_e new_state = cb_hdl->get_call_state();

        /* We have re-primed in the handler */
        if (new_state != GPI_PRIMED)
            if (cb_hdl->cleanup_callback()) {
                delete cb_hdl;
            }

    }

    return;
};

static void register_initial_callback(void)
{
    FENTER
    sim_init_cb = new VhpiStartupCbHdl(vhpi_table);
    sim_init_cb->arm_callback();
    FEXIT
}

static void register_final_callback(void)
{
    FENTER
    sim_finish_cb = new VhpiShutdownCbHdl(vhpi_table);
    sim_finish_cb->arm_callback();
    FEXIT
}

static void register_embed(void)
{
    vhpi_table = new VhpiImpl("VHPI");
    gpi_register_impl(vhpi_table);
    gpi_load_extra_libs();
}

// pre-defined VHPI registration table
void (*vhpi_startup_routines[])(void) = {
    register_embed,
    register_initial_callback,
    register_final_callback,
    0
};

// For non-VPI compliant applications that cannot find vlog_startup_routines
void vhpi_startup_routines_bootstrap(void) {
    void (*routine)(void);
    int i;
    routine = vhpi_startup_routines[0];
    for (i = 0, routine = vhpi_startup_routines[i];
         routine;
         routine = vhpi_startup_routines[++i]) {
        routine();
    }
}

}

GPI_ENTRY_POINT(vhpi, register_embed)
