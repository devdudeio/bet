#ifndef STORAGE_H
#define STORAGE_H

#include "bet.h"
#include <sqlite3.h>

#define sql_query_size 1024

void sqlite3_init_db_name();
int32_t sqlite3_check_if_table_id_exists(const char *table_id);
int32_t sqlite3_check_if_table_exists(sqlite3 *db, const char *table_name);
sqlite3 *bet_get_db_instance();
void bet_make_insert_query(int argc, char **argv, char **sql_query);
int32_t bet_run_query(char *sql_query);
void bet_create_schema();
void bet_sqlite3_init();
int32_t sqlite3_delete_dealer(char *dealer_ip);
cJSON *sqlite3_get_dealer_info_details();
cJSON *sqlite3_get_game_details(int32_t opt);
cJSON *bet_show_fail_history();
cJSON *bet_show_success_history();
int32_t bet_store_game_info_details(char *tx_id, char *table_id);
int32_t bet_insert_sc_game_info(char *tx_id, char *table_id, int32_t bh, char *tx_type);
int32_t sqlite3_get_highest_bh();
int32_t insert_player_deck_info_txid_pa_t_d(char *tx_id, char *pa, char *table_id, char *dealer_id);
int32_t update_player_deck_info_a_rG(char *tx_id);
int32_t update_player_deck_info_game_id_p_id(char *tx_id);
int32_t insert_dealer_deck_info();
int32_t insert_cashier_deck_info(char *table_id);

// Player deck info persistence for rejoin
int32_t save_player_deck_info(const char *game_id_str, const char *table_id, int32_t player_id);
int32_t load_player_deck_info(const char *game_id_str);
int32_t save_dealer_deck_info(const char *game_id_str);
int32_t load_dealer_deck_info(const char *game_id_str);

// Player local state persistence (payin_tx, decoded cards, game progress)
void init_p_local_state(void);
int32_t save_player_local_state(const char *payin_tx);
int32_t update_player_decoded_card(int32_t card_index, int32_t card_value);
int32_t load_player_local_state(const char *game_id_str, int32_t player_id);
char *get_player_payin_tx(const char *game_id_str);

// Parameterized query helpers (SQL injection safe)
int32_t bet_sql_update_c_tx_payout(const char *tx_info, const char *table_id_val);
int32_t bet_sql_insert_cashier_game_state(const char *table_id_val, const char *game_state_val);
int32_t bet_sql_update_player_tx_status(const char *tx_id_val, int new_status);
int32_t bet_sql_update_c_tx_by_payin(const char *payout_tx_id, const char *payin_tx_id);
int32_t bet_sql_insert_dealer(const char *ip);
int32_t bet_sql_update_player_tx_payout(const char *payout_tx, const char *tx_id_val, int new_status);
int32_t bet_sql_insert_dcv_tx(const char *tx_info, const char *table_id_val, const char *player_id,
	const char *msig_addr, int status, int min_cashiers);
int32_t bet_sql_update_dcv_tx_status(const char *table_id_val);
int32_t bet_sql_insert_player_tx(const char *tx_id_val, const char *table_id_val, const char *player_id,
	const char *msig_addr, int status, int min_cashiers);
int32_t bet_sql_insert_c_tx_addr(const char *payin_tx_id, const char *msig_addr, int min_notaries,
	const char *table_id_val, const char *msig_addr_nodes, int payin_status);
int32_t bet_sql_update_player_tx_payout_by_table(const char *tx_info, const char *table_id_val, int new_status);
int32_t bet_sql_insert_game_state(const char *table_name, const char *table_id_val, const char *game_state_val);

#endif /* STORAGE_H */
