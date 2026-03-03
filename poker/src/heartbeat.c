#include "bet.h"
#include "host.h"
#include "heartbeat.h"
#include "states.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

int32_t active_players = 0;
int32_t player_status[CARDS_MAXPLAYERS] = { 0 };

void bet_dcv_reset_player_status(struct privatebet_info *bet)
{
	for (int32_t i = 0; i < bet->maxplayers; i++) {
		player_status[i] = 0;
	}
}

void bet_dcv_publish_player_active_info(struct privatebet_info *bet)
{
	cJSON *active_info = NULL;
	cJSON *players_status_info = NULL;
	int active_players = 0;
	cJSON *argjson = NULL;
	struct privatebet_vars *vars = dcv_vars;

	active_info = cJSON_CreateObject();
	cJSON_AddStringToObject(active_info, "method", "active_player_info");
	players_status_info = cJSON_CreateArray();

	for (int i = 0; i < bet->maxplayers; i++) {
		cJSON_AddItemToArray(players_status_info, cJSON_CreateNumber(player_status[i]));
		if (player_status[i] == 1) {
			active_players++;
		}
	}
	if (player_status[vars->turni] == 0) {
		argjson = cJSON_CreateObject();
		cJSON_AddStringToObject(argjson, "method", "betting");
		cJSON_AddNumberToObject(argjson, "playerid", vars->turni);
		cJSON_AddNumberToObject(argjson, "round", vars->round);
		cJSON_AddNumberToObject(argjson, "pot", vars->pot);
		cJSON_AddNumberToObject(argjson, "min_amount", 0);
		cJSON_AddStringToObject(argjson, "action", "fold");
		bet_dcv_round_betting_response(argjson, bet, vars);
	}
	if (active_players < bet->maxplayers) {
		DLG_JSON(info, "Players disconnect info::%s", players_status_info);
	}
	cJSON_AddItemToObject(active_info, "player_status", players_status_info);
}

void bet_dcv_heartbeat_loop(void *_ptr)
{
	struct privatebet_info *bet = _ptr;

	while (1) {
		if (heartbeat_on == 1) {
			bet_dcv_reset_player_status(bet);
			sleep(5);
			bet_dcv_publish_player_active_info(bet);
		}
	}
}

void bet_dcv_update_player_status(cJSON *argjson)
{
	int32_t playerid;

	playerid = jint(argjson, "playerid");
	if (playerid >= 0 && playerid < CARDS_MAXPLAYERS)
		player_status[playerid] = 1;
}

void bet_dcv_heartbeat_thread(struct privatebet_info *bet)
{
	pthread_t live_thrd;

	if (OS_thread_create(&live_thrd, NULL, (void *)bet_dcv_heartbeat_loop, (void *)bet) != 0) {
		dlg_error("Error launching bet_dcv_heartbeat_loop");
		exit(-1);
	}

	if (pthread_join(live_thrd, NULL)) {
		dlg_error("Error in joining the main thread for bet_dcv_heartbeat_loop");
	}
}
