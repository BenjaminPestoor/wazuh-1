/*
 * Wazuh Module for policy monitoring
 * Copyright (C) 2015-2019, Wazuh Inc.
 * January 25, 2019.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "wmodules.h"
#include <os_net/os_net.h>
#include "shared.h"

#undef minfo
#undef mwarn
#undef merror
#undef mdebug1
#undef mdebug2

#define minfo(msg, ...) _mtinfo(WM_POLICY_MONITORING_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define mwarn(msg, ...) _mtwarn(WM_POLICY_MONITORING_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define merror(msg, ...) _mterror(WM_POLICY_MONITORING_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define mdebug1(msg, ...) _mtdebug1(WM_POLICY_MONITORING_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)
#define mdebug2(msg, ...) _mtdebug2(WM_POLICY_MONITORING_LOGTAG, __FILE__, __LINE__, __func__, msg, ##__VA_ARGS__)

static void * wm_policy_monitoring_main(wm_policy_monitoring_t * data);   // Module main function. It won't return
static void wm_policy_monitoring_destroy(wm_policy_monitoring_t * data);  // Destroy data
static int wm_policy_monitoring_start(wm_policy_monitoring_t * data);  // Start
static int wm_policy_monitoring_send_event_check(wm_policy_monitoring_t * data,cJSON *profile,cJSON *policy,char **p_alert_msg,int id,int index,char *result);  // Send check event
static void wm_policy_monitoring_read_files(wm_policy_monitoring_t * data,int id);  // Read policy monitoring files
static int wm_policy_monitoring_do_scan(OSList *plist,cJSON *profile_check,OSStore *vars,wm_policy_monitoring_t * data,int id,cJSON *policy);  // Do scan
static int wm_policy_monitoring_send_summary(wm_policy_monitoring_t * data, int scan_id,unsigned int passed, unsigned int failed,cJSON *policy);  // Send summary
static int wm_policy_monitoring_check_policy(cJSON *policy);
static void wm_policy_monitoring_summary_increment_passed();
static void wm_policy_monitoring_summary_increment_failed();
static void wm_policy_monitoring_reset_summary();
static int wm_policy_monitoring_send_alert(wm_policy_monitoring_t * data,cJSON *json_alert); // Send alert
static int wm_policy_monitoring_check_hash(OSHash *cis_db_hash,char *result,cJSON *profile);
static int wm_policy_monitoring_hash_integrity(OSHash *cis_db_hash);
static void wm_policy_monitoring_free_hash_data(char *data);

/* Extra functions */
static int wm_policy_monitoring_get_vars(cJSON *variables,OSStore *vars);
static void wm_policy_monitoring_set_condition(char *c_cond, int *condition); // Set condition
static char * wm_policy_monitoring_get_value(char *buf, int *type); // Get value
static char * wm_policy_monitoring_get_pattern(char *value); // Get pattern
static int wm_policy_monitoring_check_file(char *file, char *pattern,wm_policy_monitoring_t * data); // Check file
static int wm_policy_monitoring_pt_check_negate(const char *pattern); // Check pattern negate
static int wm_policy_monitoring_pt_matches(const char *str, char *pattern); // Check pattern match
static int wm_policy_monitoring_check_dir(const char *dir, const char *file, char *pattern,wm_policy_monitoring_t * data); // Check dir
static int wm_policy_monitoring_is_process(char *value, OSList *p_list,wm_policy_monitoring_t * data); // Check is a process

#ifdef WIN32
static int wm_policy_monitoring_is_registry(char *entry_name, char *reg_option, char *reg_value);
static char *wm_policy_monitoring_os_winreg_getkey(char *reg_entry);
static int wm_policy_monitoring_open_key(char *subkey, char *full_key_name, unsigned long arch,char *reg_option, char *reg_value);
static int wm_policy_monitoring_winreg_querykey(HKEY hKey,__attribute__((unused))char *p_key,__attribute__((unused)) char *full_key_name,char *reg_option, char *reg_value);
static char *wm_policy_monitoring_getrootdir(char *root_dir, int dir_size);
#endif

cJSON *wm_policy_monitoring_dump(const wm_policy_monitoring_t * data);     // Read config

const wm_context WM_POLICY_MONITORING_CONTEXT = {
    PM_WM_NAME,
    (wm_routine)wm_policy_monitoring_main,
    (wm_routine)wm_policy_monitoring_destroy,
    (cJSON * (*)(const void *))wm_policy_monitoring_dump
};

typedef enum _request_type{
    W_TYPE_ID,W_TYPE_IP
} _request_type_t;

static unsigned int summary_passed = 0;
static unsigned int summary_failed = 0;
OSHash *cis_db;

// Module main function. It won't return
void * wm_policy_monitoring_main(wm_policy_monitoring_t * data) {
    // If module is disabled, exit
    if (data->enabled) {
        minfo("Module started");
    } else {
        minfo("Module disabled. Exiting.");
        pthread_exit(NULL);
    }

    cis_db = OSHash_Create();
    if (!cis_db) {
        merror(LIST_ERROR);
        return (0);
    }

    OSHash_SetFreeDataPointer(cis_db, (void (*)(void *))wm_policy_monitoring_free_hash_data);

    sleep(2); /* Wait for daemons to settle */
    wm_policy_monitoring_start(data);
    
    return NULL;
}

static int wm_policy_monitoring_send_alert(wm_policy_monitoring_t * data,cJSON *json_alert)
{
    char *msg = cJSON_PrintUnformatted(json_alert);

    /* When running in context of OSSEC-HIDS, send problem to the rootcheck queue */
    if (SendMSG(data->queue, msg, WM_POLICY_MONITORING_STAMP, POLICY_MONITORING_MQ) < 0) {
        mterror(ARGV0, QUEUE_SEND);

        if ((data->queue = StartMQ(DEFAULTQPATH, WRITE)) < 0) {
            mterror_exit(ARGV0, QUEUE_FATAL, DEFAULTQPATH);
        }

        if (SendMSG(data->queue, msg, WM_POLICY_MONITORING_STAMP, POLICY_MONITORING_MQ) < 0) {
            mterror_exit(ARGV0, QUEUE_FATAL, DEFAULTQPATH);
        }
    }

    os_free(msg);

    return (0);
}

static int wm_policy_monitoring_start(wm_policy_monitoring_t * data) {

    int status = 0;
    time_t time_start = 0;
    time_t time_end = 0;
    time_t time_sleep = 0;

    if (!data->scan_on_start) {
        time_start = time(NULL);

        if (data->scan_day) {
            do {
                status = check_day_to_scan(data->scan_day, data->scan_time);
                if (status == 0) {
                    time_sleep = get_time_to_hour(data->scan_time);
                } else {
                    wm_delay(1000); // Sleep one second to avoid an infinite loop
                    time_sleep = get_time_to_hour("00:00");
                }

                mtdebug2(WM_POLICY_MONITORING_LOGTAG, "Sleeping for %d seconds", (int)time_sleep);
                wm_delay(1000 * time_sleep);

            } while (status < 0);

        } else if (data->scan_wday >= 0) {

            time_sleep = get_time_to_day(data->scan_wday, data->scan_time);
            mtinfo(WM_POLICY_MONITORING_LOGTAG, "Waiting for turn to evaluate.");
            mtdebug2(WM_POLICY_MONITORING_LOGTAG, "Sleeping for %d seconds", (int)time_sleep);
            wm_delay(1000 * time_sleep);

        } else if (data->scan_time) {

            time_sleep = get_time_to_hour(data->scan_time);
            mtinfo(WM_POLICY_MONITORING_LOGTAG, "Waiting for turn to evaluate.");
            mtdebug2(WM_POLICY_MONITORING_LOGTAG, "Sleeping for %d seconds", (int)time_sleep);
            wm_delay(1000 * time_sleep);

        } else if (data->next_time == 0 || data->next_time > time_start) {

            // On first run, take into account the interval of time specified
            time_sleep = data->next_time == 0 ?
                         (time_t)data->interval :
                         data->next_time - time_start;

            mtinfo(WM_POLICY_MONITORING_LOGTAG, "Waiting for turn to evaluate.");
            mtdebug2(WM_POLICY_MONITORING_LOGTAG, "Sleeping for %ld seconds", (long)time_sleep);
            wm_delay(1000 * time_sleep);

        }
    }

    while(1) {
        // Get time and execute
        time_start = time(NULL);

        mtinfo(WM_POLICY_MONITORING_LOGTAG, "Starting evaluation.");

        // Set unique ID for each scan

#ifndef WIN32
        int id = os_random();
        if (id < 0)
            id = -id;
#else
        unsigned int id1 = os_random();
        unsigned int id2 = os_random();

        char random_id[OS_MAXSTR];
        snprintf(random_id, OS_MAXSTR - 1, "%u%u", id1, id2);

        int id = atoi(random_id);
        if (id < 0)
            id = -id;
#endif

        /* Send scan start message */
        cJSON *start_scan = cJSON_CreateObject();
        cJSON_AddStringToObject(start_scan, "type", "scan-started");
        cJSON_AddStringToObject(start_scan, "message", "Policy monitoring scan started");
        cJSON_AddNumberToObject(start_scan, "time", (long)time_start);
        cJSON_AddNumberToObject(start_scan, "scan_id", id);
        wm_policy_monitoring_send_alert(data,start_scan);
        cJSON_Delete(start_scan);

        /* Do scan for every profile file */
        wm_policy_monitoring_read_files(data,id);

        wm_delay(1000); // Avoid infinite loop when execution fails
        time_sleep = time(NULL) - time_start;
        time_end = time(NULL);

        /* Send scan ending message */
        cJSON *end_scan = cJSON_CreateObject();
        cJSON_AddStringToObject(end_scan, "type", "scan-ended");
        cJSON_AddStringToObject(end_scan, "message", "Policy monitoring scan finished");
        cJSON_AddNumberToObject(end_scan, "time", (long)time_end);
        cJSON_AddNumberToObject(end_scan, "scan_id", id);
        wm_policy_monitoring_send_alert(data,end_scan);
        minfo("Ended policy monitoring scan.");
        cJSON_Delete(end_scan);

        if (data->scan_day) {
            int interval = 0, i = 0;
            status = 0;
            interval = data->interval / 60;   // interval in num of months

            do {
                status = check_day_to_scan(data->scan_day, data->scan_time);
                if (status == 0) {
                    time_sleep = get_time_to_hour(data->scan_time);
                    i++;
                } else {
                    wm_delay(1000);
                    time_sleep = get_time_to_hour("00:00");     // Sleep until the start of the next day
                }

                mtdebug2(WM_POLICY_MONITORING_LOGTAG, "Sleeping for %d seconds", (int)time_sleep);
                wm_delay(1000 * time_sleep);

            } while ((status < 0) && (i < interval));

        } else {

            if (data->scan_wday >= 0) {
                time_sleep = get_time_to_day(data->scan_wday, data->scan_time);
                time_sleep += WEEK_SEC * ((data->interval / WEEK_SEC) - 1);
                data->next_time = (time_t)time_sleep + time_start;
            } else if (data->scan_time) {
                time_sleep = get_time_to_hour(data->scan_time);
                time_sleep += DAY_SEC * ((data->interval / DAY_SEC) - 1);
                data->next_time = (time_t)time_sleep + time_start;
            } else if ((time_t)data->interval >= time_sleep) {
                time_sleep = data->interval - time_sleep;
                data->next_time = data->interval + time_start;
            } else {
                mterror(WM_POLICY_MONITORING_LOGTAG, "Interval overtaken.");
                time_sleep = data->next_time = 0;
            }

            mtdebug2(WM_POLICY_MONITORING_LOGTAG, "Sleeping for %d seconds", (int)time_sleep);
            wm_delay(1000 * time_sleep);
        }
    }

    return 0;
}

static void wm_policy_monitoring_read_files(wm_policy_monitoring_t * data,int id) {
    FILE *fp;
    int i = 0;

    /* Read every policy monitoring file */
    for(i = 0; data->profile[i]; i++) {
        char path[PATH_MAX];
        OSStore *vars = NULL;
        cJSON * object = NULL;

#ifdef WIN32
        sprintf(path,"%s\\%s",SHAREDCFG_DIR, data->profile[i]);
#elif CLIENT
        sprintf(path,"%s/%s",DEFAULTDIR SHAREDCFG_DIR, data->profile[i]);
#else
        sprintf(path,"%s/%s",DEFAULTDIR ROOTCHECKCFG_DIR, data->profile[i]);
#endif
        fp = fopen(path,"r");

        if(!fp) {
            mwarn("Policy file not found '%s'",path);
            goto next;
        }

        /* Yaml parsing */
        yaml_document_t document;
       
        if (yaml_parse_file(path, &document)) {
            merror("Policy file could not be parsed '%s'",path);
            goto next;
        }

        if (object = yaml2json(&document), !object) {
            merror("Transforming yaml to json '%s'",path);
            goto next;
        }

        yaml_document_delete(&document);

        OSList *plist = w_os_get_process_list();
        cJSON *policy = cJSON_GetObjectItem(object, "policy");
        cJSON *variables = cJSON_GetObjectItem(object, "variables");
        cJSON *profiles = cJSON_GetObjectItem(object, "checks");

        if(wm_policy_monitoring_check_policy(policy)) {
            merror("Check your 'policy' field");
            goto next;
        }

        if(!profiles){
            merror("Obtaining 'checks' from json");
            goto next;
        }

        vars = OSStore_Create();

        if( wm_policy_monitoring_get_vars(variables,vars) != 0 ){
            goto next;
        }

        wm_policy_monitoring_do_scan(plist,profiles,vars,data,id,policy);
        wm_policy_monitoring_send_summary(data,id,summary_passed,summary_failed,policy);
        wm_policy_monitoring_reset_summary();
        w_del_plist(plist);
        mtinfo(WM_POLICY_MONITORING_LOGTAG, "Evaluation finished for policy '%s'.",data->profile[i]);

next:
        if(fp){
            fclose(fp);
        }

        if(object) {
            cJSON_free(object);
        }

        if(vars) {
            OSStore_Free(vars);
        }
    }
}

static int wm_policy_monitoring_check_policy(cJSON *policy) {
    int retval;
    cJSON *id;
    cJSON *name;

    retval = 1;

    if(!policy) {
        return retval;
    }

    id = cJSON_GetObjectItem(policy, "id");
    if(!id) {
        merror("Field 'id' not found on policy");
        return retval;
    }

    name = cJSON_GetObjectItem(policy, "name");
    if(!name) {
        merror("Field 'name' not found on policy");
        return retval;
    }

    retval = 0;
    return retval;
}

static int wm_policy_monitoring_do_scan(OSList *p_list,cJSON *profile_check,OSStore *vars,wm_policy_monitoring_t * data,int id,cJSON *policy) {

    int type = 0, condition = 0;
    char *nbuf;
    char buf[OS_SIZE_1024 + 2];
    char root_dir[OS_SIZE_1024 + 2];
    char final_file[2048 + 1];
    char *value;
    char *name = NULL;
    cJSON *c_title = NULL;
    cJSON *c_condition = NULL;

    /* Initialize variables */
    memset(buf, '\0', sizeof(buf));
    memset(root_dir, '\0', sizeof(root_dir));
    memset(final_file, '\0', sizeof(final_file));

#ifdef WIN32
    /* Get Windows rootdir */
    wm_policy_monitoring_getrootdir(root_dir, sizeof(root_dir) - 1);
    if (root_dir[0] == '\0') {
        mterror(ARGV0, INVALID_ROOTDIR);
    }
#endif
    cJSON *profile = NULL;

    cJSON_ArrayForEach(profile,profile_check){

        c_title = cJSON_GetObjectItem(profile, "title");
        c_condition = cJSON_GetObjectItem(profile, "condition");
        cJSON *p_checks = cJSON_GetObjectItem(profile, "rules");

        /* Get first name */
        if(c_title) {
            name = c_title->valuestring;
        } else {
            name = NULL;
        }

        /* Get condition */
        if(c_condition) {
            wm_policy_monitoring_set_condition(c_condition->valuestring,&condition);
        } else {
            wm_policy_monitoring_set_condition("invalid",&condition);
        }

        if (name == NULL || condition == WM_POLICY_MONITORING_COND_INV) {
            merror(WM_POLICY_MONITORING_INVALID_RKCL_NAME, name );
            goto clean_return;
        }
    
        if(p_checks){
            cJSON *p_check;

            int g_found = 0;
            int not_found = 0;

            cJSON_ArrayForEach(p_check,p_checks)
            {
                mtdebug2(ARGV0, "Checking entry: '%s'.", name);

                int negate = 0;
                int found = 0;
                value = NULL;
                nbuf = p_check->valuestring;
            
                /* Get value to look for */
                value = wm_policy_monitoring_get_value(nbuf, &type);
                if (value == NULL) {
                    mterror(ARGV0, WM_POLICY_MONITORING_INVALID_RKCL_VALUE, nbuf);
                    goto clean_return;
                }

                /* Get negate value */
                if (*value == '!') {
                    negate = 1;
                    value++;
                }

                /* Check for a file */
                if (type == WM_POLICY_MONITORING_TYPE_FILE) {
                    char *pattern = NULL;
                    char *f_value = NULL;

                    pattern = wm_policy_monitoring_get_pattern(value);
                    f_value = value;

                    /* Get any variable */
                    if (value[0] == '$') {
                        f_value = (char *) OSStore_Get(vars, value);
                        if (!f_value) {
                            mterror(ARGV0, WM_POLICY_MONITORING_INVALID_RKCL_VAR, value);
                            continue;
                        }
                    }

    #ifdef WIN32
                    else if (value[0] == '\\') {
                        final_file[0] = '\0';
                        final_file[sizeof(final_file) - 1] = '\0';

                        snprintf(final_file, sizeof(final_file) - 2, "%s%s",
                                root_dir, value);
                        f_value = final_file;
                    } else {
                        final_file[0] = '\0';
                        final_file[sizeof(final_file) - 1] = '\0';

                        ExpandEnvironmentStrings(value, final_file,
                                                sizeof(final_file) - 2);
                        f_value = final_file;
                    }
    #endif

                    mtdebug2(ARGV0, "Checking file: '%s'.", f_value);
                    if (wm_policy_monitoring_check_file(f_value, pattern,data)) {
                        mtdebug2(ARGV0, "Found file.");
                        found = 1;
                    }
                }

    #ifdef WIN32
                /* Check for a registry entry */
                else if (type == WM_POLICY_MONITORING_TYPE_REGISTRY) {
                    char *entry = NULL;
                    char *pattern = NULL;

                    /* Look for additional entries in the registry
                    * and a pattern to match.
                    */
                    entry = wm_policy_monitoring_get_pattern(value);
                    if (entry) {
                        pattern = wm_policy_monitoring_get_pattern(entry);
                    }

                    mtdebug2(ARGV0, "Checking registry: '%s'.", value);
                    if (wm_policy_monitoring_is_registry(value, entry, pattern)) {
                        mtdebug2(ARGV0, "Found registry.");
                        found = 1;
                    }

                }
    #endif
                /* Check for a directory */
                else if (type == WM_POLICY_MONITORING_TYPE_DIR) {
                    char *file = NULL;
                    char *pattern = NULL;
                    char *f_value = NULL;
                    char *dir = NULL;

                    file = wm_policy_monitoring_get_pattern(value);
                    if (!file) {
                        mterror(ARGV0, WM_POLICY_MONITORING_INVALID_RKCL_VAR, value);
                        continue;
                    }

                    pattern = wm_policy_monitoring_get_pattern(file);

                    /* Get any variable */
                    if (value[0] == '$') {
                        f_value = (char *) OSStore_Get(vars, value);
                        if (!f_value) {
                            mterror(ARGV0, WM_POLICY_MONITORING_INVALID_RKCL_VAR, value);
                            continue;
                        }
                    } else {
                        f_value = value;
                    }

                    /* Check for multiple comma separated directories */
                    dir = f_value;
                    f_value = strchr(dir, ',');
                    if (f_value) {
                        *f_value = '\0';
                    }

                    while (dir) {

                        mdebug2("Checking dir: %s", dir);

                        short is_nfs = IsNFS(dir);
                        if( is_nfs == 1 && data->skip_nfs ) {
                            mdebug1("rootcheck.skip_nfs enabled and %s is flagged as NFS.", dir);
                        }
                        else {
                            mdebug2("%s => is_nfs=%d, skip_nfs=%d", dir, is_nfs, data->skip_nfs);

                            if (wm_policy_monitoring_check_dir(dir, file, pattern,data)) {
                                mdebug2("Found dir.");
                                found = 1;
                            }
                        }

                        if (f_value) {
                            *f_value = ',';
                            f_value++;

                            dir = f_value;

                            f_value = strchr(dir, ',');
                            if (f_value) {
                                *f_value = '\0';
                            }
                        } else {
                            dir = NULL;
                        }
                    }
                }

                /* Check for a process */
                else if (type == WM_POLICY_MONITORING_TYPE_PROCESS) {
                    mdebug2("Checking process: '%s'.", value);
                    if (wm_policy_monitoring_is_process(value, p_list,data)) {
                        mdebug2("Found process.");
                        found = 1;
                    }
                }

                /* Switch the values if ! is present */
                if (negate) {
                    if (found) {
                        found = 0;
                    } else {
                        found = 1;
                    }
                }

                /* Check the conditions */
                if (condition & WM_POLICY_MONITORING_COND_ANY) {
                    mdebug2("Condition ANY.");
                    if (found) {
                        g_found = 1;
                        wm_policy_monitoring_summary_increment_failed();
                    } else {
                        wm_policy_monitoring_summary_increment_passed();
                    }
                } else if (condition & WM_POLICY_MONITORING_COND_NON) {
                    mdebug2("Condition NON.");
                    if (!found && (not_found != -1)) {
                        mdebug2("Condition NON setze not_found=1.");
                        not_found = 1;
                        wm_policy_monitoring_summary_increment_failed();
                    } else {
                        not_found = -1;
                        wm_policy_monitoring_summary_increment_passed();
                    }
                } else {
                    /* Condition for ALL */
                    mdebug2("Condition ALL.");
                    if (found && (g_found != -1)) {
                        g_found = 1;
                        wm_policy_monitoring_summary_increment_failed();
                    } else {
                        g_found = -1;
                        wm_policy_monitoring_summary_increment_passed();
                    }
                }
            }

            if (condition & WM_POLICY_MONITORING_COND_NON) {
                if (not_found == -1){ g_found = 0;} else {g_found = 1;}
            }

            /* Alert if necessary */
            if (g_found == 1) {
                int j = 0;
                char **p_alert_msg = data->alert_msg;

                while (1) {
                    if ((type == WM_POLICY_MONITORING_TYPE_DIR) || (j == 0)) {
                        if(wm_policy_monitoring_check_hash(cis_db,"fail",profile)) {
                            wm_policy_monitoring_send_event_check(data,profile,policy,p_alert_msg,id,j,"fail");
                        }
                    }

                    if (p_alert_msg[j]) {
                        free(p_alert_msg[j]);
                        p_alert_msg[j] = NULL;
                        j++;

                        if (!p_alert_msg[j]) {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            } else {
                int j = 0;
                char **p_alert_msg = data->alert_msg;
                while (1) {
                    if ((type == WM_POLICY_MONITORING_TYPE_DIR) || (j == 0)) {

                        if(wm_policy_monitoring_check_hash(cis_db,"passed",profile)) {
                            wm_policy_monitoring_send_event_check(data,profile,policy,p_alert_msg,id,j,"passed");
                        }
                    }

                    if (p_alert_msg[j]) {
                        free(p_alert_msg[j]);
                        p_alert_msg[j] = NULL;
                        j++;

                        if (!p_alert_msg[j]) {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                j = 0;
                while (data->alert_msg[j]) {
                    free(data->alert_msg[j]);
                    data->alert_msg[j] = NULL;
                    j++;
                }

                /* Check if this entry is required for the rest of the file */
                if (condition & WM_POLICY_MONITORING_COND_REQ) {
                    goto clean_return;
                }
            }

            /* End if we don't have anything else */
            if (!nbuf) {
                goto clean_return;
            }

            /* Clean up name */
            if (name) {
                free(name);
                name = NULL;
            }
        }
    }

/* Clean up memory */
clean_return:
    if (name) {
        free(name);
        name = NULL;
    }

    return 0;

}

static void wm_policy_monitoring_set_condition(char *c_cond, int *condition) {
    /* Get condition */
    if (strcmp(c_cond, "all") == 0) {
        *condition |= WM_POLICY_MONITORING_COND_ALL;
    } else if (strcmp(c_cond, "any") == 0) {
        *condition |= WM_POLICY_MONITORING_COND_ANY;
    } else if (strcmp(c_cond, "none") == 0) {
        *condition |= WM_POLICY_MONITORING_COND_NON;
    } else if (strcmp(c_cond, "any required") == 0) {
        *condition |= WM_POLICY_MONITORING_COND_ANY;
        *condition |= WM_POLICY_MONITORING_COND_REQ;
    } else if (strcmp(c_cond, "all required") == 0) {
        *condition |= WM_POLICY_MONITORING_COND_ALL;
        *condition |= WM_POLICY_MONITORING_COND_REQ;
    } else {
        *condition = WM_POLICY_MONITORING_COND_INV;
    }
}

static int wm_policy_monitoring_get_vars(cJSON *variables,OSStore *vars) {

    cJSON *variable;
    cJSON_ArrayForEach(variable,variables){

        /* If not a variable, return 0 */
        if (*variable->string != '$') {
            merror(WM_POLICY_MONITORING_INVALID_RKCL_VAR, variable->string);
            return (0);
        }

        /* Remove semicolon from the end */
        char *tmp = strchr(variable->valuestring, ';');
        if (tmp) {
            *tmp = '\0';
        } else {
            return (-1);
        }

        char * var_value;
        os_strdup(variable->valuestring,var_value);
        OSStore_Put(vars, variable->string, var_value);
    }

    return 0;
}

static char *wm_policy_monitoring_get_value(char *buf, int *type)
{
    char *tmp_str;
    char *value;

    /* Zero type before using it to make sure return is valid
     * in case of error.
     */
    *type = 0;

    value = strchr(buf, ':');
    if (value == NULL) {
        return (NULL);
    }

    *value = '\0';
    value++;

    tmp_str = strchr(value, ';');
    if (tmp_str == NULL) {
        return (NULL);
    }
    *tmp_str = '\0';

    /* Get types - removing negate flag (using later) */
    if (*buf == '!') {
        buf++;
    }

    if (strcmp(buf, "f") == 0) {
        *type = WM_POLICY_MONITORING_TYPE_FILE;
    } else if (strcmp(buf, "r") == 0) {
        *type = WM_POLICY_MONITORING_TYPE_REGISTRY;
    } else if (strcmp(buf, "p") == 0) {
        *type = WM_POLICY_MONITORING_TYPE_PROCESS;
    } else if (strcmp(buf, "d") == 0) {
        *type = WM_POLICY_MONITORING_TYPE_DIR;
    } else {
        return (NULL);
    }

    return (value);
}

static char *wm_policy_monitoring_get_pattern(char *value)
{
    while (*value != '\0') {
        if ((*value == ' ') && (value[1] == '-') &&
                (value[2] == '>') && (value[3] == ' ')) {
            *value = '\0';
            value += 4;

            return (value);
        }
        value++;
    }

    return (NULL);
}

static int wm_policy_monitoring_check_file(char *file, char *pattern,wm_policy_monitoring_t * data)
{
    char *split_file;
    int full_negate = 0;
    int pt_result = 0;
    FILE *fp;
    char buf[OS_SIZE_2048 + 1];

    if (file == NULL) {
        return (0);
    }

    /* Check if the file is divided */
    split_file = strchr(file, ',');
    if (split_file) {
        *split_file = '\0';
        split_file++;
    }

    /* Get each file */
    do {
        /* If we don't have a pattern, just check if the file/dir is there */
        if (pattern == NULL) {
            if (w_is_file(file)) {
                int i = 0;
                char _b_msg[OS_SIZE_1024 + 1];

                _b_msg[OS_SIZE_1024] = '\0';
                snprintf(_b_msg, OS_SIZE_1024, " File: %s.",
                         file);

                /* Already present */
                if (w_is_str_in_array(data->alert_msg, _b_msg)) {
                    return (1);
                }

                while (data->alert_msg[i] && (i < 255)) {
                    i++;
                }

                if (!data->alert_msg[i]) {
                    os_strdup(_b_msg, data->alert_msg[i]);
                }

                return (1);
            }
        } else {
            full_negate = wm_policy_monitoring_pt_check_negate(pattern);
            /* Check for content in the file */
            mdebug2("Checking file: %s", file);
            fp = fopen(file, "r");
            if (fp) {

                mdebug2("Starting new file: %s", file);
                buf[OS_SIZE_2048] = '\0';
                while (fgets(buf, OS_SIZE_2048, fp) != NULL) {
                    char *nbuf;

                    /* Remove end of line */
                    nbuf = strchr(buf, '\n');
                    if (nbuf) {
                        *nbuf = '\0';
                    }
#ifdef WIN32
                    /* Remove end of line */
                    nbuf = strchr(buf, '\r');
                    if (nbuf) {
                        *nbuf = '\0';
                    }
#endif
                    /* Matched */
                    pt_result = wm_policy_monitoring_pt_matches(buf, pattern);
                    mdebug2("Buf == \"%s\"", buf);
                    mdebug2("Pattern == \"%s\"", pattern);
                    mdebug2("pt_result == %d and full_negate == %d", pt_result, full_negate);
                    if ((pt_result == 1 && full_negate == 0) ) {
                        mdebug1("Alerting file %s on line %s", file, buf);
                        int i = 0;
                        char _b_msg[OS_SIZE_1024 + 1];

                        /* Close the file before dealing with the alert */
                        fclose(fp);

                        /* Generate the alert itself */
                        _b_msg[OS_SIZE_1024] = '\0';
                        snprintf(_b_msg, OS_SIZE_1024, " File: %s.",
                                 file);

                        /* Already present */
                        if (w_is_str_in_array(data->alert_msg, _b_msg)) {
                            return (1);
                        }

                        while (data->alert_msg[i] && (i < 255)) {
                            i++;
                        }

                        if (!data->alert_msg[i]) {
                            os_strdup(_b_msg, data->alert_msg[i]);
                        }

                        return (1);
                    } else if ((pt_result == 0 && full_negate == 1) ) {
                        /* Found a full+negate match so no longer need to search
                         * break out of loop and make sure the full negate does
                         * not alert.
                         */
                        mdebug2("Found a complete match for full_negate");
                        full_negate = 0;
                        break;
                    }
                }

                fclose(fp);

                if (full_negate == 1) {
                    mdebug2("Full_negate alerting - file %s", file);
                    int i = 0;
                    char _b_msg[OS_SIZE_1024 + 1];

                    /* Generate the alert itself */
                    _b_msg[OS_SIZE_1024] = '\0';
                    snprintf(_b_msg, OS_SIZE_1024, " File: %s.",
                             file);

                    /* Already present */
                    if (w_is_str_in_array(data->alert_msg, _b_msg)) {
                        return (1);
                    }

                    while (data->alert_msg[i] && (i < 255)) {
                        i++;
                    }

                    if (!data->alert_msg[i]) {
                        os_strdup(_b_msg, data->alert_msg[i]);
                    }

                    return (1);
                }
            }
        }

        if (split_file) {
            file = split_file;
            split_file = strchr(split_file, ',');
            if (split_file) {
                split_file++;
            }
        }


    } while (split_file);

    return (0);
}

/* Check if the pattern is all negate values */
static int wm_policy_monitoring_pt_check_negate(const char *pattern)
{
    char *mypattern = NULL;
    os_strdup(pattern, mypattern);
    char *tmp_pt = mypattern;
    char *tmp_pattern = mypattern;

    while (tmp_pt != NULL) {
        /* First look for " && " */
        tmp_pt = strchr(tmp_pattern, ' ');
        if (tmp_pt && tmp_pt[1] == '&' && tmp_pt[2] == '&' && tmp_pt[3] == ' ') {
            *tmp_pt = '\0';
            tmp_pt += 4;
        } else {
            tmp_pt = NULL;
        }

        if (*tmp_pattern != '!') {
            free(mypattern);
            return 0;
        }

        tmp_pattern = tmp_pt;
    }

    mdebug2("Pattern: %s is fill_negate", pattern);
    free(mypattern);
    return (1);
}

static int wm_policy_monitoring_pt_matches(const char *str, char *pattern)
{
    int neg = 0;
    int ret_code = 0;
    char *tmp_pt = pattern;
    char *tmp_ret = NULL;

    if (str == NULL) {
        return (0);
    }

    while (tmp_pt != NULL) {
        /* First look for " && " */
        tmp_pt = strchr(pattern, ' ');
        if (tmp_pt && tmp_pt[1] == '&' && tmp_pt[2] == '&' && tmp_pt[3] == ' ') {
            /* Mark pointer to clean it up */
            tmp_ret = tmp_pt;

            *tmp_pt = '\0';
            tmp_pt += 4;
        } else {
            tmp_pt = NULL;
        }

        /* Check for negate values */
        neg = 0;
        ret_code = 0;
        if (*pattern == '!') {
            pattern++;
            neg = 1;
        }

        /* Do the actual comparison */
        if (strncasecmp(pattern, "=:", 2) == 0) {
            pattern += 2;
            if (strcasecmp(pattern, str) == 0) {
                ret_code = 1;
            }
        } else if (strncasecmp(pattern, "r:", 2) == 0) {
            pattern += 2;
            if (OS_Regex(pattern, str)) {
                mdebug2("Pattern: %s matches %s.", pattern, str);
                ret_code = 1;
            }
        } else if (strncasecmp(pattern, "<:", 2) == 0) {
            pattern += 2;
            if (strcmp(pattern, str) < 0) {
                ret_code = 1;
            }
        } else if (strncasecmp(pattern, ">:", 2) == 0) {
            pattern += 2;
            if (strcmp(pattern, str) > 0) {
                ret_code = 1;
            }
        } else {
#ifdef WIN32
            char final_file[2048 + 1];

            /* Try to get Windows variable */
            if (*pattern == '%') {
                final_file[0] = '\0';
                final_file[2048] = '\0';

                ExpandEnvironmentStrings(pattern, final_file, 2047);
            } else {
                strncpy(final_file, pattern, 2047);
            }

            /* Compare against the expanded variable */
            if (strcasecmp(final_file, str) == 0) {
                ret_code = 1;
            }
#else
            if (strcasecmp(pattern, str) == 0) {
                ret_code = 1;
            }
#endif
        }
        /* Fix tmp_ret entry */
        if (tmp_ret != NULL) {
            *tmp_ret = ' ';
            tmp_ret = NULL;
        }

        /* If we have "!", return true if we don't match */
        if (neg == 1) {
            if (ret_code) {
                ret_code = 0;
                break;
            }
        } else {
            if (!ret_code) {
                ret_code = 0;
                break;
            }
        }

        ret_code = 1;
        pattern = tmp_pt;
    }

    return (ret_code);
}

static int wm_policy_monitoring_check_dir(const char *dir, const char *file, char *pattern,wm_policy_monitoring_t * data)
{
    int ret_code = 0;
    char f_name[PATH_MAX + 2];
    struct dirent *entry;
    struct stat statbuf_local;
    DIR *dp = NULL;

    f_name[PATH_MAX + 1] = '\0';

    dp = opendir(dir);
    if (!dp) {
        return (0);
    }

    while ((entry = readdir(dp)) != NULL) {
        /* Ignore . and ..  */
        if ((strcmp(entry->d_name, ".") == 0) ||
                (strcmp(entry->d_name, "..") == 0)) {
            continue;
        }

        /* Create new file + path string */
        snprintf(f_name, PATH_MAX + 1, "%s/%s", dir, entry->d_name);

        /* Check if the read entry matches the provided file name */
        if (strncasecmp(file, "r:", 2) == 0) {
            if (OS_Regex(file + 2, entry->d_name)) {
                if (wm_policy_monitoring_check_file(f_name, pattern,data)) {
                    ret_code = 1;
                }
            }
        } else {
            /* ... otherwise try without regex */
            if (OS_Match2(file, entry->d_name)) {
                if (wm_policy_monitoring_check_file(f_name, pattern,data)) {
                    ret_code = 1;
                }
            }
        }

        /* Check if file is a directory */
        if (lstat(f_name, &statbuf_local) == 0) {
            if (S_ISDIR(statbuf_local.st_mode)) {
                if (wm_policy_monitoring_check_dir(f_name, file, pattern,data)) {
                    ret_code = 1;
                }
            }
        }
    }

    closedir(dp);
    return (ret_code);

}

/* Check if a process is running */
static int wm_policy_monitoring_is_process(char *value, OSList *p_list,wm_policy_monitoring_t * data)
{
    OSListNode *l_node;
    if (p_list == NULL) {
        return (0);
    }
    if (!value) {
        return (0);
    }

    l_node = OSList_GetFirstNode(p_list);
    while (l_node) {
        W_Proc_Info *pinfo;

        pinfo = (W_Proc_Info *)l_node->data;

        /* Check if value matches */
        if (wm_policy_monitoring_pt_matches(pinfo->p_path, value)) {
            int i = 0;
            char _b_msg[OS_SIZE_1024 + 1];

            _b_msg[OS_SIZE_1024] = '\0';

            snprintf(_b_msg, OS_SIZE_1024, " Process: %s.",
                     pinfo->p_path);

            /* Already present */
            if (w_is_str_in_array(data->alert_msg, _b_msg)) {
                return (1);
            }

            while (data->alert_msg[i] && (i < 255)) {
                i++;
            }

            if (!data->alert_msg[i]) {
                os_strdup(_b_msg, data->alert_msg[i]);
            }

            return (1);
        }

        l_node = OSList_GetNextNode(p_list);
    }

    return (0);
}

// Destroy data
void wm_policy_monitoring_destroy(wm_policy_monitoring_t * data) {
    os_free(data);
}

#ifdef WIN32
static int wm_policy_monitoring_is_registry(char *entry_name, char *reg_option, char *reg_value) {
    char *rk;

    rk = wm_policy_monitoring_os_winreg_getkey(entry_name);
    if (wm_policy_monitoring_sub_tree == NULL || rk == NULL) {
        mterror(ARGV0, SK_INV_REG, entry_name);
        return (0);
    }

    return wm_policy_monitoring_open_key(rk, entry_name, KEY_WOW64_32KEY, reg_option, reg_value) || wm_policy_monitoring_open_key(rk, entry_name, KEY_WOW64_64KEY, reg_option, reg_value);
}
static char *wm_policy_monitoring_os_winreg_getkey(char *reg_entry)
{
    char *ret = NULL;
    char *tmp_str;

    /* Get only the sub tree first */
    tmp_str = strchr(reg_entry, '\\');
    if (tmp_str) {
        *tmp_str = '\0';
        ret = tmp_str + 1;
    }

    /* Set sub tree */
    if ((strcmp(reg_entry, "HKEY_LOCAL_MACHINE") == 0) ||
            (strcmp(reg_entry, "HKLM") == 0)) {
        wm_policy_monitoring_sub_tree = HKEY_LOCAL_MACHINE;
    } else if (strcmp(reg_entry, "HKEY_CLASSES_ROOT") == 0) {
        wm_policy_monitoring_sub_tree = HKEY_CLASSES_ROOT;
    } else if (strcmp(reg_entry, "HKEY_CURRENT_CONFIG") == 0) {
        wm_policy_monitoring_sub_tree = HKEY_CURRENT_CONFIG;
    } else if (strcmp(reg_entry, "HKEY_USERS") == 0) {
        wm_policy_monitoring_sub_tree = HKEY_USERS;
    } else if ((strcmp(reg_entry, "HKCU") == 0) ||
               (strcmp(reg_entry, "HKEY_CURRENT_USER") == 0)) {
        wm_policy_monitoring_sub_tree = HKEY_CURRENT_USER;
    } else {
        /* Set sub tree to null */
        wm_policy_monitoring_sub_tree = NULL;

        /* Return tmp_str to the previous value */
        if (tmp_str && (*tmp_str == '\0')) {
            *tmp_str = '\\';
        }
        return (NULL);
    }

    /* Check if ret has nothing else */
    if (ret && (*ret == '\0')) {
        ret = NULL;
    }

    /* Fixing tmp_str and the real name of the registry */
    if (tmp_str && (*tmp_str == '\0')) {
        *tmp_str = '\\';
    }

    return (ret);
}

static int wm_policy_monitoring_open_key(char *subkey, char *full_key_name, unsigned long arch,
                         char *reg_option, char *reg_value)
{
    int ret = 1;
    HKEY oshkey;

    if (RegOpenKeyEx(wm_policy_monitoring_sub_tree, subkey, 0, KEY_READ | arch, &oshkey) != ERROR_SUCCESS) {
        return (0);
    }

    /* If option is set, return the value of query key */
    if (reg_option) {
        ret = wm_policy_monitoring_winreg_querykey(oshkey, subkey, full_key_name,
                                   reg_option, reg_value);
    }

    RegCloseKey(oshkey);
    return (ret);
}

static int wm_policy_monitoring_winreg_querykey(HKEY hKey,
        __attribute__((unused))char *p_key,
        __attribute__((unused)) char *full_key_name,
                         char *reg_option, char *reg_value)
{
    int rc;
    DWORD i, j;

    /* QueryInfo and EnumKey variables */
    TCHAR class_name_b[MAX_PATH + 1];
    DWORD class_name_s = MAX_PATH;

    /* Number of sub keys */
    DWORD subkey_count = 0;

    /* Number of values */
    DWORD value_count;

    /* Variables for RegEnumValue */
    TCHAR value_buffer[MAX_VALUE_NAME + 1];
    TCHAR data_buffer[MAX_VALUE_NAME + 1];
    DWORD value_size;
    DWORD data_size;

    /* Data type for RegEnumValue */
    DWORD data_type = 0;

    /* Storage var */
    char var_storage[MAX_VALUE_NAME + 1];

    /* Initialize the memory for some variables */
    class_name_b[0] = '\0';
    class_name_b[MAX_PATH] = '\0';

    /* We use the class_name, subkey_count and the value count */
    rc = RegQueryInfoKey(hKey, class_name_b, &class_name_s, NULL,
                         &subkey_count, NULL, NULL, &value_count,
                         NULL, NULL, NULL, NULL);
    if (rc != ERROR_SUCCESS) {
        return (0);
    }

    /* Get values (if available) */
    if (value_count) {
        char *mt_data;

        /* Clear the values for value_size and data_size */
        value_buffer[MAX_VALUE_NAME] = '\0';
        data_buffer[MAX_VALUE_NAME] = '\0';
        var_storage[MAX_VALUE_NAME] = '\0';

        /* Get each value */
        for (i = 0; i < value_count; i++) {
            value_size = MAX_VALUE_NAME;
            data_size = MAX_VALUE_NAME;

            value_buffer[0] = '\0';
            data_buffer[0] = '\0';
            var_storage[0] = '\0';

            rc = RegEnumValue(hKey, i, value_buffer, &value_size,
                              NULL, &data_type, (LPBYTE)data_buffer, &data_size);

            /* No more values available */
            if (rc != ERROR_SUCCESS) {
                break;
            }

            /* Check if no value name is specified */
            if (value_buffer[0] == '\0') {
                value_buffer[0] = '@';
                value_buffer[1] = '\0';
            }

            /* Check if the entry name matches the reg_option */
            if (strcasecmp(value_buffer, reg_option) != 0) {
                continue;
            }

            /* If a value is not present and the option matches,
             * we can return ok
             */
            if (!reg_value) {
                return (1);
            }

            /* Write value into a string */
            switch (data_type) {
                    int size_available;

                case REG_SZ:
                case REG_EXPAND_SZ:
                    snprintf(var_storage, MAX_VALUE_NAME, "%s", data_buffer);
                    break;
                case REG_MULTI_SZ:
                    /* Printing multiple strings */
                    size_available = MAX_VALUE_NAME - 3;
                    mt_data = data_buffer;

                    while (*mt_data) {
                        if (size_available > 2) {
                            strncat(var_storage, mt_data, size_available);
                            strncat(var_storage, " ", 2);
                            size_available = MAX_VALUE_NAME -
                                             (strlen(var_storage) + 2);
                        }
                        mt_data += strlen(mt_data) + 1;
                    }

                    break;
                case REG_DWORD:
                    snprintf(var_storage, MAX_VALUE_NAME,
                             "%x", (unsigned int)*data_buffer);
                    break;
                default:
                    size_available = MAX_VALUE_NAME - 2;
                    for (j = 0; j < data_size; j++) {
                        char tmp_c[12];

                        snprintf(tmp_c, 12, "%02x",
                                 (unsigned int)data_buffer[j]);

                        if (size_available > 2) {
                            strncat(var_storage, tmp_c, size_available);
                            size_available = MAX_VALUE_NAME -
                                             (strlen(var_storage) + 2);
                        }
                    }
                    break;
            }

            /* Check if value matches */
            if (wm_policy_monitoring_pt_matches(var_storage, reg_value)) {
                return (1);
            }

            return (0);
        }
    }

    return (0);
}

static char *wm_policy_monitoring_getrootdir(char *root_dir, int dir_size)
{
    char final_file[2048 + 1];
    char *tmp;

    final_file[0] = '\0';
    final_file[2048] = '\0';

    ExpandEnvironmentStrings("%WINDIR%", final_file, 2047);

    tmp = strchr(final_file, '\\');
    if (tmp) {
        *tmp = '\0';
        strncpy(root_dir, final_file, dir_size);
        return (root_dir);
    }

    return (NULL);
}
#endif

static int wm_policy_monitoring_send_summary(wm_policy_monitoring_t * data, int scan_id,unsigned int passed, unsigned int failed,cJSON *policy) {
    cJSON *json_summary = cJSON_CreateObject();

    cJSON_AddStringToObject(json_summary, "type", "summary");
    cJSON_AddNumberToObject(json_summary, "scan_id", scan_id);

    cJSON *name = cJSON_GetObjectItem(policy,"name");
    cJSON *description = cJSON_GetObjectItem(policy,"description");
    cJSON *references = cJSON_GetObjectItem(policy,"references");

    cJSON_AddStringToObject(json_summary, "name", name->valuestring);

    if(description) {
        cJSON_AddStringToObject(json_summary, "description", description->valuestring);
    }
  
    if(references) {
        cJSON_AddStringToObject(json_summary, "references", references ? references->valuestring : NULL );
    }
   
    cJSON_AddNumberToObject(json_summary, "passed", passed);
    cJSON_AddNumberToObject(json_summary, "failed", failed);

    float passedf = passed;
    float failedf = failed;
    float score = ((passedf/(failedf+passedf)))* 100;
    
    cJSON_AddNumberToObject(json_summary, "score", score);

    wm_policy_monitoring_send_alert(data,json_summary);
    cJSON_Delete(json_summary);

    return 0;
}

static int wm_policy_monitoring_send_event_check(wm_policy_monitoring_t * data,cJSON *profile,cJSON *policy,char **p_alert_msg,int id,int index,char *result) {
    cJSON *json_alert = cJSON_CreateObject();
    cJSON_AddStringToObject(json_alert, "type", "check");
    cJSON_AddNumberToObject(json_alert, "id", id);

    cJSON *name = cJSON_GetObjectItem(policy,"name");
    cJSON_AddStringToObject(json_alert, "profile", name->valuestring);
    
    
    cJSON *check = cJSON_CreateObject();
    cJSON *pm_id = cJSON_GetObjectItem(profile, "id");
    cJSON *title = cJSON_GetObjectItem(profile, "title");
    cJSON *cis_control = cJSON_GetObjectItem(profile, "cis_control");
    cJSON *description = cJSON_GetObjectItem(profile, "description");
    cJSON *rationale = cJSON_GetObjectItem(profile, "rationale");
    cJSON *remediation = cJSON_GetObjectItem(profile, "remediation");
    cJSON *default_value = cJSON_GetObjectItem(profile, "default_value");

    if(!pm_id) {
        merror("No 'id' field found on check");
        return 1; 
    }

    cJSON_AddNumberToObject(check, "id", pm_id->valueint);

    if(title){
        cJSON_AddStringToObject(check, "title", title->valuestring);
    } else {
        merror("No 'title' field found on check '%d'",pm_id->valueint);
        return 1;
    }

    if(cis_control){
        char value[OS_SIZE_128];
        
        if(cis_control->valuedouble){
            sprintf(value, "%lf", cis_control->valuedouble);
        } else if (cis_control->valueint) {
            sprintf(value, "%d", cis_control->valueint);
        } else {
            sprintf(value, "%s", cis_control->valuestring);
        }

        cJSON_AddStringToObject(check, "cis_control", value);
    }

    if(description){
        cJSON_AddStringToObject(check, "description", description->valuestring);
    }

    if(rationale){
        cJSON_AddStringToObject(check, "rationale", rationale->valuestring);
    }

    if(remediation){
        cJSON_AddStringToObject(check, "remediation", remediation->valuestring);
    }

    if(default_value){
        cJSON_AddStringToObject(check, "default_value", default_value->valuestring);
    }

    cJSON *compliances = cJSON_GetObjectItem(profile, "compliance");

    if(compliances) {
        cJSON *add_compliances = cJSON_CreateObject();
        cJSON *compliance;

        cJSON_ArrayForEach(compliance,compliances)
        {
            if(compliance->child->valuestring){
                cJSON_AddStringToObject(add_compliances,compliance->child->string,compliance->child->valuestring);
            } else if(compliance->child->valueint) {
                cJSON_AddNumberToObject(add_compliances,compliance->child->string,compliance->child->valueint);
            }
        }

        cJSON_AddItemToObject(check,"compliance",add_compliances);
    }
    
    cJSON *references = cJSON_GetObjectItem(profile, "references");

    if(references) {
        cJSON *add_references = cJSON_CreateArray();
        cJSON *reference;
        cJSON_ArrayForEach(reference,references)
        {
            cJSON_AddItemToArray(add_references,cJSON_CreateString(reference->valuestring));
        }
        cJSON_AddItemToObject(check,"reference",add_references);
    }

    // Get File or Process from alert
    if(p_alert_msg[index]) {
        char *alert_file = strstr(p_alert_msg[index],"File:");
        if(alert_file){
            alert_file+= 5;
            *alert_file = '\0';
            alert_file++;
            cJSON_AddStringToObject(check, "file", alert_file);
        } else {
            char *alert_process = strstr(p_alert_msg[index],"Process:");
            if(alert_process){
                alert_process+= 8;
                *alert_process = '\0';
                alert_process++;
                cJSON_AddStringToObject(check, "process", alert_process);
            }
        }
    } else {
        cJSON_AddStringToObject(check, "file", "\0");
    }
    
    cJSON_AddStringToObject(check, "result", result);
    cJSON_AddItemToObject(json_alert,"check",check);

    wm_policy_monitoring_send_alert(data,json_alert);
    cJSON_Delete(json_alert);

    return 0;
}

static int wm_policy_monitoring_check_hash(OSHash *cis_db_hash,char *result,cJSON *profile) {
    char *hashed_result = NULL;
    char id_hashed[OS_SIZE_128];
    int ret_add = 0;
    cJSON *pm_id = cJSON_GetObjectItem(profile, "id");

    if(!pm_id) {
        return 0;
    }

    sprintf(id_hashed, "%d", pm_id->valueint);

    hashed_result = OSHash_Get(cis_db_hash,id_hashed);

    if(hashed_result){
        if(strcmp(result,hashed_result) == 0) {
            return 0;
        } else {
            if (ret_add = OSHash_Update(cis_db_hash,id_hashed,strdup(result)), ret_add != 1) {
                merror("Unable to add cis id to db: %d", pm_id->valueint);
                return 0;
            }
            return 1;
        }
    } else {
        if (ret_add = OSHash_Add(cis_db_hash,id_hashed,strdup(result)), ret_add != 2) {
            merror("Unable to add cis id to db: %d", pm_id->valueint);
            return 0;
        }
        return 1;
    }
}

static void wm_policy_monitoring_free_hash_data(char *data) {
    os_free(data);
}

static int wm_policy_monitoring_hash_integrity(OSHash *cis_db_hash) {

    return 0;
}

static void wm_policy_monitoring_summary_increment_passed() {
    summary_passed++;
}

static void wm_policy_monitoring_summary_increment_failed() {
    summary_failed++;
}

static void wm_policy_monitoring_reset_summary() {
    summary_failed = 0;
    summary_passed = 0;
}

cJSON *wm_policy_monitoring_dump(const wm_policy_monitoring_t *data) {
    cJSON *root = cJSON_CreateObject();
    cJSON *wm_wd = cJSON_CreateObject();

    cJSON_AddStringToObject(wm_wd, "enabled", data->enabled ? "yes" : "no");
    cJSON_AddStringToObject(wm_wd, "scan_on_start", data->scan_on_start ? "yes" : "no");
    cJSON_AddStringToObject(wm_wd, "skip_nfs", data->skip_nfs ? "yes" : "no");
    if (data->interval) cJSON_AddNumberToObject(wm_wd, "interval", data->interval);
    if (data->scan_day) cJSON_AddNumberToObject(wm_wd, "day", data->scan_day);

    switch (data->scan_wday) {
        case 0:
            cJSON_AddStringToObject(wm_wd, "wday", "sunday");
            break;
        case 1:
            cJSON_AddStringToObject(wm_wd, "wday", "monday");
            break;
        case 2:
            cJSON_AddStringToObject(wm_wd, "wday", "tuesday");
            break;
        case 3:
            cJSON_AddStringToObject(wm_wd, "wday", "wednesday");
            break;
        case 4:
            cJSON_AddStringToObject(wm_wd, "wday", "thursday");
            break;
        case 5:
            cJSON_AddStringToObject(wm_wd, "wday", "friday");
            break;
        case 6:
            cJSON_AddStringToObject(wm_wd, "wday", "saturday");
            break;
        default:
            break;
    }
    if (data->scan_time) cJSON_AddStringToObject(wm_wd, "time", data->scan_time);

    if (data->profile && *data->profile) {
        cJSON *profiles = cJSON_CreateArray();
        int i;
        for (i=0;data->profile[i];i++) {
            cJSON *profile = cJSON_CreateObject();
            cJSON_AddStringToObject(profile,"profile",data->profile[i]);
            cJSON_AddItemToArray(profiles, profile);
        }
        cJSON_AddItemToObject(wm_wd,"profiles",profiles);
    }

    cJSON_AddItemToObject(root,"policy-monitoring",wm_wd);

    
    return root;
}