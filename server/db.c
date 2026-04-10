#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <pthread.h>
#include "cJSON.h"

sqlite3 *db;
pthread_mutex_t db_write_mutex = PTHREAD_MUTEX_INITIALIZER;

void init_db() {
    int rc = sqlite3_open("collab_files.db", &db);
    if (rc) {
        fprintf(stderr, "Can't open database: %s\n", sqlite3_errmsg(db));
        exit(1);
    }

    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);

    const char *sql_schema = 
        "CREATE TABLE IF NOT EXISTS files ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "content TEXT NOT NULL"
        ");"
        
        "INSERT OR IGNORE INTO files (id, name, content) VALUES (1, 'main.js', '// Start coding main.js...');";

    char *errmsg = 0;
    rc = sqlite3_exec(db, sql_schema, 0, 0, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
}

int create_file(const char *name) {
    pthread_mutex_lock(&db_write_mutex);
    int new_id = -1;
    const char *sql = "INSERT INTO files (name, content) VALUES (?, '// new file');";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) == SQLITE_DONE) {
            new_id = (int)sqlite3_last_insert_rowid(db);
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_write_mutex);
    return new_id;
}

void delete_file(int id) {
    pthread_mutex_lock(&db_write_mutex);
    const char *sql = "DELETE FROM files WHERE id = ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_write_mutex);
}

void rename_file(int id, const char *new_name) {
    pthread_mutex_lock(&db_write_mutex);
    const char *sql = "UPDATE files SET name = ? WHERE id = ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_write_mutex);
}

void save_file(int id, const char *content) {
    pthread_mutex_lock(&db_write_mutex);
    const char *sql = "UPDATE files SET content = ? WHERE id = ?;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, content, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_write_mutex);
}

char* get_all_files_json(void) {
    cJSON *files_array = cJSON_CreateArray();
    
    const char *sql = "SELECT id, name, content FROM files;";
    sqlite3_stmt *stmt;
    
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            cJSON *file_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(file_obj, "id", sqlite3_column_int(stmt, 0));
            
            const unsigned char *name = sqlite3_column_text(stmt, 1);
            cJSON_AddStringToObject(file_obj, "name", name ? (const char *)name : "");
            
            const unsigned char *content = sqlite3_column_text(stmt, 2);
            cJSON_AddStringToObject(file_obj, "content", content ? (const char *)content : "");
            
            cJSON_AddItemToArray(files_array, file_obj);
        }
        sqlite3_finalize(stmt);
    }
    
    char *json_str = cJSON_PrintUnformatted(files_array);
    cJSON_Delete(files_array);
    return json_str; // Must be freed by caller
}
