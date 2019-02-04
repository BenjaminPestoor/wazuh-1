/*
 * Wazuh SQLite integration
 * Copyright (C) 2015-2019, Wazuh Inc.
 * June 06, 2016.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */

#include "wdb.h"

static const char *SQL_INSERT_PM = "INSERT INTO pm_event (date_first, date_last, log, pci_dss, cis) VALUES (datetime(?, 'unixepoch', 'localtime'), datetime(?, 'unixepoch', 'localtime'), ?, ?, ?);";
static const char *SQL_UPDATE_PM = "UPDATE pm_event SET date_last = datetime(?, 'unixepoch', 'localtime') WHERE log = ?;";
static const char *SQL_DELETE_PM = "DELETE FROM pm_event;";

/* Get PCI_DSS requirement from log string */
static char* get_pci_dss(const char *string);

/* Get CIS requirement from log string */
char* get_cis(const char *string);

/* Insert policy monitoring entry. Returns ID on success or -1 on error. */
int wdb_insert_pm(sqlite3 *db, const rk_event_t *event) {
    sqlite3_stmt *stmt = NULL;
    int result;
    char *pci_dss;
    char *cis;

    if (wdb_prepare(db, SQL_INSERT_PM, -1, &stmt, NULL)) {
        mdebug1("SQLite: %s", sqlite3_errmsg(db));
        return -1;
    }

    pci_dss = get_pci_dss(event->log);
    cis = get_cis(event->log);

    sqlite3_bind_int(stmt, 1, event->date_first);
    sqlite3_bind_int(stmt, 2, event->date_last);
    sqlite3_bind_text(stmt, 3, event->log, -1, NULL);
    sqlite3_bind_text(stmt, 4, pci_dss, -1, NULL);
    sqlite3_bind_text(stmt, 5, cis, -1, NULL);

    result = wdb_step(stmt) == SQLITE_DONE ? (int)sqlite3_last_insert_rowid(db) : -1;
    sqlite3_finalize(stmt);
    free(pci_dss);
    free(cis);
    return result;
}

/* Update policy monitoring last date. Returns number of affected rows on success or -1 on error. */
int wdb_update_pm(sqlite3 *db, const rk_event_t *event) {
    sqlite3_stmt *stmt = NULL;
    int result;

    if (wdb_prepare(db, SQL_UPDATE_PM, -1, &stmt, NULL)) {
        mdebug1("SQLite: %s", sqlite3_errmsg(db));
        return -1;
    }

    sqlite3_bind_int(stmt, 1, event->date_last);
    sqlite3_bind_text(stmt, 2, event->log, -1, NULL);

    result = wdb_step(stmt) == SQLITE_DONE ? sqlite3_changes(db) : -1;
    sqlite3_finalize(stmt);
    return result;
}

/* Delete PM events of an agent. Returns 0 on success or -1 on error. */
int wdb_delete_pm(int id) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    char *name = id ? wdb_agent_name(id) : strdup("localhost");
    int result;

    if (!name)
        return -1;

    db = wdb_open_agent(id, name);
    free(name);

    if (!db)
        return -1;

    if (wdb_prepare(db, SQL_DELETE_PM, -1, &stmt, NULL)) {
        mdebug1("SQLite: %s", sqlite3_errmsg(db));
        sqlite3_close_v2(db);
        return -1;
    }

    result = wdb_step(stmt) == SQLITE_DONE ? sqlite3_changes(db) : -1;
    sqlite3_finalize(stmt);
    wdb_vacuum(db);
    sqlite3_close_v2(db);
    return result;
}

/* Look for a policy monitoring entry in Wazuh DB. Returns 1 if found, 0 if not, or -1 on error. (new) */
int wdb_policy_monitoring_find(wdb_t * wdb, char * pm_id, char * output) {

    if (!wdb->transaction && wdb_begin2(wdb) < 0){
        mdebug1("cannot begin transaction");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;

    if (wdb_stmt_cache(wdb, WDB_STMT_PM_FIND) < 0) {
        mdebug1("cannot cache statement");
        return -1;
    }

    stmt = wdb->stmt[WDB_STMT_PM_FIND];

    sqlite3_bind_text(stmt, 1, pm_id, -1, NULL);

    switch (sqlite3_step(stmt)) {
        case SQLITE_ROW:
            snprintf(output,OS_MAXSTR,"%s",(char*)sqlite3_column_text(stmt, 1));
            return 1;
            break;
        case SQLITE_DONE:
            return 0;
            break;
        default:
            merror(" at sqlite3_step(): %s", sqlite3_errmsg(wdb->db));
            return -1;
    }
}

/* Insert policy monitoring entry. Returns 0 on success or -1 on error (new) */
int wdb_policy_monitoring_save(wdb_t * wdb, char * pm_id, char * title, char * description, char * file,char * reference, char * pci_dss, char * cis, char * result) {

    if (!wdb->transaction && wdb_begin2(wdb) < 0){
        mdebug1("at wdb_rootcheck_save(): cannot begin transaction");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;

    if (wdb_stmt_cache(wdb, WDB_STMT_PM_INSERT) < 0) {
        mdebug1("at wdb_rootcheck_save(): cannot cache statement");
        return -1;
    }

    stmt = wdb->stmt[WDB_STMT_PM_INSERT];

    sqlite3_bind_text(stmt, 1, pm_id, -1, NULL);
    sqlite3_bind_text(stmt, 2, title, -1, NULL);
    sqlite3_bind_text(stmt, 3, description, -1, NULL);
    sqlite3_bind_text(stmt, 4, file, -1, NULL);
    sqlite3_bind_text(stmt, 5, reference, -1, NULL);
    sqlite3_bind_text(stmt, 6, pci_dss, -1, NULL);
    sqlite3_bind_text(stmt, 7, cis, -1, NULL);
    sqlite3_bind_text(stmt, 8, result, -1, NULL);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
        free(pci_dss);
        free(cis);
        return 0;
    } else {
        merror("sqlite3_step(): %s", sqlite3_errmsg(wdb->db));
        free(pci_dss);
        free(cis);
        return -1;
    }
}

/* Insert policy monitoring entry. Returns 0 on success or -1 on error (new) */
int wdb_policy_monitoring_scan_info_save(wdb_t * wdb, char * module, int start_scan, int end_scan, int scan_id) {

     if (!wdb->transaction && wdb_begin2(wdb) < 0){
        mdebug1("cannot begin transaction");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;

    if (wdb_stmt_cache(wdb, WDB_STMT_PM_SCAN_INFO_INSERT) < 0) {
        mdebug1("cannot cache statement");
        return -1;
    }

    stmt = wdb->stmt[WDB_STMT_PM_SCAN_INFO_INSERT];

    sqlite3_bind_text(stmt, 1, module, -1, NULL);
    sqlite3_bind_int(stmt, 2, start_scan);
    sqlite3_bind_int(stmt, 3, end_scan);
    sqlite3_bind_int(stmt, 4, scan_id);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
        return 0;
    } else {
        merror("sqlite3_step(): %s", sqlite3_errmsg(wdb->db));
        return -1;
    }
}

int wdb_policy_monitoring_scan_info_update(wdb_t * wdb, char * module, int end_scan){
    if (!wdb->transaction && wdb_begin2(wdb) < 0){
        mdebug1("at wdb_rootcheck_update(): cannot begin transaction");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;

    if (wdb_stmt_cache(wdb, WDB_STMT_PM_SCAN_INFO_UPDATE) < 0) {
        mdebug1("at wdb_rootcheck_update(): cannot cache statement");
        return -1;
    }

    stmt = wdb->stmt[WDB_STMT_PM_SCAN_INFO_UPDATE];

    sqlite3_bind_int(stmt, 1, end_scan);
    sqlite3_bind_text(stmt, 2, module, -1, NULL);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
        return sqlite3_changes(wdb->db);
    } else {
        merror("at wdb_rootcheck_update(): sqlite3_step(): %s", sqlite3_errmsg(wdb->db));
        return -1;
    }
}

/* Update a policy monitoring entry. Returns affected rows on success or -1 on error (new) */
int wdb_policy_monitoring_update(wdb_t * wdb, char * result, char * pm_id) {

    if (!wdb->transaction && wdb_begin2(wdb) < 0){
        mdebug1("at wdb_rootcheck_update(): cannot begin transaction");
        return -1;
    }

    sqlite3_stmt *stmt = NULL;

    if (wdb_stmt_cache(wdb, WDB_STMT_PM_UPDATE) < 0) {
        mdebug1("at wdb_rootcheck_update(): cannot cache statement");
        return -1;
    }

    stmt = wdb->stmt[WDB_STMT_PM_UPDATE];

    sqlite3_bind_text(stmt, 1, result,-1, NULL);
    sqlite3_bind_text(stmt, 2, pm_id, -1, NULL);

    if (sqlite3_step(stmt) == SQLITE_DONE) {
        return sqlite3_changes(wdb->db);
    } else {
        merror("at wdb_rootcheck_update(): sqlite3_step(): %s", sqlite3_errmsg(wdb->db));
        return -1;
    }
}

/* Delete PM events of all agents */
void wdb_delete_pm_all() {
    int *agents = wdb_get_all_agents();
    int i;

    if (agents) {
        wdb_delete_pm(0);

        for (i = 0; agents[i] >= 0; i++)
            wdb_delete_pm(agents[i]);

        free(agents);
    }
}

/* Get PCI_DSS requirement from log string */
char* get_pci_dss(const char *string) {
    size_t length;
    char *out = strstr(string, "{PCI_DSS: ");

    if (out) {
        out += 10;
        length = strcspn(out, "}");

        if (length < strlen(out)) {
            out = strdup(out);
            out[length] = '\0';
            return out;
        }
    }
        return NULL;
}

/* Get CIS requirement from log string */
char* get_cis(const char *string) {
    size_t length;
    char *out = strstr(string, "{CIS: ");

    if (out) {
        out += 6;
        length = strcspn(out, "}");

        if (length < strlen(out)) {
            out = strdup(out);
            out[length] = '\0';
            return out;
        }
    }
        return NULL;
}
