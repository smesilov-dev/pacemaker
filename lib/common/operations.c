/*
 * Copyright 2004-2020 the Pacemaker project contributors
 *
 * The version control history for this file may have further details.
 *
 * This source code is licensed under the GNU Lesser General Public License
 * version 2.1 or later (LGPLv2.1+) WITHOUT ANY WARRANTY.
 */

#include <crm_internal.h>

#ifndef _GNU_SOURCE
#  define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <crm/crm.h>
#include <crm/lrmd.h>
#include <crm/msg_xml.h>
#include <crm/common/xml.h>
#include <crm/common/util.h>

/*!
 * \brief Generate an operation key (RESOURCE_ACTION_INTERVAL)
 *
 * \param[in] rsc_id       ID of resource being operated on
 * \param[in] op_type      Operation name
 * \param[in] interval_ms  Operation interval
 *
 * \return Newly allocated memory containing operation key as string
 *
 * \note This function asserts on errors, so it will never return NULL.
 *       The caller is responsible for freeing the result with free().
 */
char *
pcmk__op_key(const char *rsc_id, const char *op_type, guint interval_ms)
{
    CRM_ASSERT(rsc_id != NULL);
    CRM_ASSERT(op_type != NULL);
    return crm_strdup_printf(PCMK__OP_FMT, rsc_id, op_type, interval_ms);
}

gboolean
parse_op_key(const char *key, char **rsc_id, char **op_type, guint *interval_ms)
{
    char *notify = NULL;
    char *mutable_key = NULL;
    char *mutable_key_ptr = NULL;
    size_t len = 0, offset = 0;
    unsigned long long ch = 0;
    guint local_interval_ms = 0;

    // Initialize output variables in case of early return
    if (rsc_id) {
        *rsc_id = NULL;
    }
    if (op_type) {
        *op_type = NULL;
    }
    if (interval_ms) {
        *interval_ms = 0;
    }

    CRM_CHECK(key && *key, return FALSE);

    // Parse interval at end of string
    len = strlen(key);
    offset = len - 1;
    while ((offset > 0) && isdigit(key[offset])) {
        ch = key[offset] - '0';
        for (int digits = len - offset; digits > 1; --digits) {
            ch = ch * 10;
        }
        local_interval_ms += ch;
        offset--;
    }
    crm_trace("Operation key '%s' has interval %ums", key, local_interval_ms);
    if (interval_ms) {
        *interval_ms = local_interval_ms;
    }

    CRM_CHECK((offset != (len - 1)) && (key[offset] == '_'), return FALSE);

    mutable_key = strndup(key, offset);
    offset--;

    while (offset > 0 && key[offset] != '_') {
        offset--;
    }

    CRM_CHECK(key[offset] == '_',
              free(mutable_key); return FALSE);

    mutable_key_ptr = mutable_key + offset + 1;

    crm_trace("  Action: %s", mutable_key_ptr);
    if (op_type) {
        *op_type = strdup(mutable_key_ptr);
    }

    mutable_key[offset] = 0;
    offset--;

    notify = strstr(mutable_key, "_post_notify");
    if (notify && safe_str_eq(notify, "_post_notify")) {
        notify[0] = 0;
    }

    notify = strstr(mutable_key, "_pre_notify");
    if (notify && safe_str_eq(notify, "_pre_notify")) {
        notify[0] = 0;
    }

    crm_trace("  Resource: %s", mutable_key);
    if (rsc_id) {
        *rsc_id = mutable_key;
    } else {
        free(mutable_key);
    }

    return TRUE;
}

char *
pcmk__notify_key(const char *rsc_id, const char *notify_type,
                 const char *op_type)
{
    CRM_CHECK(rsc_id != NULL, return NULL);
    CRM_CHECK(op_type != NULL, return NULL);
    CRM_CHECK(notify_type != NULL, return NULL);
    return crm_strdup_printf("%s_%s_notify_%s_0",
                             rsc_id, notify_type, op_type);
}

/*!
 * \brief Parse a transition magic string into its constituent parts
 *
 * \param[in]  magic          Magic string to parse (must be non-NULL)
 * \param[out] uuid           If non-NULL, where to store copy of parsed UUID
 * \param[out] transition_id  If non-NULL, where to store parsed transition ID
 * \param[out] action_id      If non-NULL, where to store parsed action ID
 * \param[out] op_status      If non-NULL, where to store parsed result status
 * \param[out] op_rc          If non-NULL, where to store parsed actual rc
 * \param[out] target_rc      If non-NULL, where to stored parsed target rc
 *
 * \return TRUE if key was valid, FALSE otherwise
 * \note If uuid is supplied and this returns TRUE, the caller is responsible
 *       for freeing the memory for *uuid using free().
 */
gboolean
decode_transition_magic(const char *magic, char **uuid, int *transition_id, int *action_id,
                        int *op_status, int *op_rc, int *target_rc)
{
    int res = 0;
    char *key = NULL;
    gboolean result = TRUE;
    int local_op_status = -1;
    int local_op_rc = -1;

    CRM_CHECK(magic != NULL, return FALSE);

#ifdef SSCANF_HAS_M
    res = sscanf(magic, "%d:%d;%ms", &local_op_status, &local_op_rc, &key);
#else
    key = calloc(1, strlen(magic) - 3); // magic must have >=4 other characters
    CRM_ASSERT(key);
    res = sscanf(magic, "%d:%d;%s", &local_op_status, &local_op_rc, key);
#endif
    if (res == EOF) {
        crm_err("Could not decode transition information '%s': %s",
                magic, pcmk_strerror(errno));
        result = FALSE;
    } else if (res < 3) {
        crm_warn("Transition information '%s' incomplete (%d of 3 expected items)",
                 magic, res);
        result = FALSE;
    } else {
        if (op_status) {
            *op_status = local_op_status;
        }
        if (op_rc) {
            *op_rc = local_op_rc;
        }
        result = decode_transition_key(key, uuid, transition_id, action_id,
                                       target_rc);
    }
    free(key);
    return result;
}

char *
pcmk__transition_key(int transition_id, int action_id, int target_rc,
                     const char *node)
{
    CRM_CHECK(node != NULL, return NULL);
    return crm_strdup_printf("%d:%d:%d:%-*s",
                             action_id, transition_id, target_rc, 36, node);
}

/*!
 * \brief Parse a transition key into its constituent parts
 *
 * \param[in]  key            Transition key to parse (must be non-NULL)
 * \param[out] uuid           If non-NULL, where to store copy of parsed UUID
 * \param[out] transition_id  If non-NULL, where to store parsed transition ID
 * \param[out] action_id      If non-NULL, where to store parsed action ID
 * \param[out] target_rc      If non-NULL, where to stored parsed target rc
 *
 * \return TRUE if key was valid, FALSE otherwise
 * \note If uuid is supplied and this returns TRUE, the caller is responsible
 *       for freeing the memory for *uuid using free().
 */
gboolean
decode_transition_key(const char *key, char **uuid, int *transition_id, int *action_id,
                      int *target_rc)
{
    int local_transition_id = -1;
    int local_action_id = -1;
    int local_target_rc = -1;
    char local_uuid[37] = { '\0' };

    // Initialize any supplied output arguments
    if (uuid) {
        *uuid = NULL;
    }
    if (transition_id) {
        *transition_id = -1;
    }
    if (action_id) {
        *action_id = -1;
    }
    if (target_rc) {
        *target_rc = -1;
    }

    CRM_CHECK(key != NULL, return FALSE);
    if (sscanf(key, "%d:%d:%d:%36s", &local_action_id, &local_transition_id,
               &local_target_rc, local_uuid) != 4) {
        crm_err("Invalid transition key '%s'", key);
        return FALSE;
    }
    if (strlen(local_uuid) != 36) {
        crm_warn("Invalid UUID '%s' in transition key '%s'", local_uuid, key);
    }
    if (uuid) {
        *uuid = strdup(local_uuid);
        CRM_ASSERT(*uuid);
    }
    if (transition_id) {
        *transition_id = local_transition_id;
    }
    if (action_id) {
        *action_id = local_action_id;
    }
    if (target_rc) {
        *target_rc = local_target_rc;
    }
    return TRUE;
}

/*!
 * \internal
 * \brief Remove XML attributes not needed for operation digest
 *
 * \param[in,out] param_set  XML with operation parameters
 */
void
pcmk__filter_op_for_digest(xmlNode *param_set)
{
    char *key = NULL;
    char *timeout = NULL;
    guint interval_ms = 0;

    const char *attr_filter[] = {
        XML_ATTR_ID,
        XML_ATTR_CRM_VERSION,
        XML_LRM_ATTR_OP_DIGEST,
        XML_LRM_ATTR_TARGET,
        XML_LRM_ATTR_TARGET_UUID,
        "pcmk_external_ip"
    };

    const int meta_len = strlen(CRM_META);

    if (param_set == NULL) {
        return;
    }

    // Remove the specific attributes listed in attr_filter
    for (int lpc = 0; lpc < DIMOF(attr_filter); lpc++) {
        xml_remove_prop(param_set, attr_filter[lpc]);
    }

    key = crm_meta_name(XML_LRM_ATTR_INTERVAL_MS);
    if (crm_element_value_ms(param_set, key, &interval_ms) != pcmk_ok) {
        interval_ms = 0;
    }
    free(key);

    key = crm_meta_name(XML_ATTR_TIMEOUT);
    timeout = crm_element_value_copy(param_set, key);

    // Remove all CRM_meta_* attributes
    for (xmlAttrPtr xIter = param_set->properties; xIter != NULL; ) {
        const char *prop_name = (const char *) (xIter->name);

        xIter = xIter->next;

        // @TODO Why is this case-insensitive?
        if (strncasecmp(prop_name, CRM_META, meta_len) == 0) {
            xml_remove_prop(param_set, prop_name);
        }
    }

    if ((interval_ms != 0) && (timeout != NULL)) {
        // Add the timeout back, it's useful for recurring operation digests
        crm_xml_add(param_set, key, timeout);
    }

    free(timeout);
    free(key);
}

int
rsc_op_expected_rc(lrmd_event_data_t * op)
{
    int rc = 0;

    if (op && op->user_data) {
        decode_transition_key(op->user_data, NULL, NULL, NULL, &rc);
    }
    return rc;
}

gboolean
did_rsc_op_fail(lrmd_event_data_t * op, int target_rc)
{
    switch (op->op_status) {
        case PCMK_LRM_OP_CANCELLED:
        case PCMK_LRM_OP_PENDING:
            return FALSE;

        case PCMK_LRM_OP_NOTSUPPORTED:
        case PCMK_LRM_OP_TIMEOUT:
        case PCMK_LRM_OP_ERROR:
        case PCMK_LRM_OP_NOT_CONNECTED:
        case PCMK_LRM_OP_INVALID:
            return TRUE;

        default:
            if (target_rc != op->rc) {
                return TRUE;
            }
    }

    return FALSE;
}

/*!
 * \brief Create a CIB XML element for an operation
 *
 * \param[in] parent         If not NULL, make new XML node a child of this one
 * \param[in] prefix         Generate an ID using this prefix
 * \param[in] task           Operation task to set
 * \param[in] interval_spec  Operation interval to set
 * \param[in] timeout        If not NULL, operation timeout to set
 *
 * \return New XML object on success, NULL otherwise
 */
xmlNode *
crm_create_op_xml(xmlNode *parent, const char *prefix, const char *task,
                  const char *interval_spec, const char *timeout)
{
    xmlNode *xml_op;

    CRM_CHECK(prefix && task && interval_spec, return NULL);

    xml_op = create_xml_node(parent, XML_ATTR_OP);
    crm_xml_set_id(xml_op, "%s-%s-%s", prefix, task, interval_spec);
    crm_xml_add(xml_op, XML_LRM_ATTR_INTERVAL, interval_spec);
    crm_xml_add(xml_op, "name", task);
    if (timeout) {
        crm_xml_add(xml_op, XML_ATTR_TIMEOUT, timeout);
    }
    return xml_op;
}

/*!
 * \brief Check whether an operation requires resource agent meta-data
 *
 * \param[in] rsc_class  Resource agent class (or NULL to skip class check)
 * \param[in] op         Operation action (or NULL to skip op check)
 *
 * \return TRUE if operation needs meta-data, FALSE otherwise
 * \note At least one of rsc_class and op must be specified.
 */
bool
crm_op_needs_metadata(const char *rsc_class, const char *op)
{
    /* Agent meta-data is used to determine whether a reload is possible, and to
     * evaluate versioned parameters -- so if this op is not relevant to those
     * features, we don't need the meta-data.
     */

    CRM_CHECK(rsc_class || op, return FALSE);

    if (rsc_class
        && is_not_set(pcmk_get_ra_caps(rsc_class), pcmk_ra_cap_params)) {
        /* Meta-data is only needed for resource classes that use parameters */
        return FALSE;
    }

    /* Meta-data is only needed for these actions */
    if (!pcmk__str_eq(op, CRMD_ACTION_START, pcmk__str_null_matches)
        && strcmp(op, CRMD_ACTION_STATUS)
        && strcmp(op, CRMD_ACTION_PROMOTE)
        && strcmp(op, CRMD_ACTION_DEMOTE)
        && strcmp(op, CRMD_ACTION_RELOAD)
        && strcmp(op, CRMD_ACTION_MIGRATE)
        && strcmp(op, CRMD_ACTION_MIGRATED)
        && strcmp(op, CRMD_ACTION_NOTIFY)) {
        return FALSE;
    }

    return TRUE;
}
