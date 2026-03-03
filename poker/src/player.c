#include "bet.h"
#include "player.h"
#include "client.h"
#include "err.h"
#include "misc.h"
#include "commands.h"
#include "deck.h"
#include "game.h"
#include "config.h"
#include "print.h"
#include "poker_vdxf.h"
#include "storage.h"
#include "vdxf.h"
#include "gui.h"
#include <time.h>
#include <errno.h>

extern int32_t g_start_block;
struct p_deck_info_struct p_deck_info;

// Current betting context for GUI handler (set by player_handle_betting, read by client.c)
char g_gui_betting_action[32] = {0};  // "small_blind", "big_blind", "round_betting"
double g_gui_betting_min_amount = 0.0;

// Card names for display
static const char *card_names[52] = {
	"2♣", "3♣", "4♣", "5♣", "6♣", "7♣", "8♣", "9♣", "10♣", "J♣", "Q♣", "K♣", "A♣",
	"2♦", "3♦", "4♦", "5♦", "6♦", "7♦", "8♦", "9♦", "10♦", "J♦", "Q♦", "K♦", "A♦",
	"2♥", "3♥", "4♥", "5♥", "6♥", "7♥", "8♥", "9♥", "10♥", "J♥", "Q♥", "K♥", "A♥",
	"2♠", "3♠", "4♠", "5♠", "6♠", "7♠", "8♠", "9♠", "10♠", "J♠", "Q♠", "K♠", "A♠"
};

static const char *get_card_name(int32_t card_value) {
	if (card_value >= 0 && card_value < 52) return card_names[card_value];
	return "??";
}

static const char *get_card_type_name(int32_t card_type) {
	switch (card_type) {
		case 1: return "HOLE CARD";
		case 2: return "FLOP 1";
		case 3: return "FLOP 2";
		case 4: return "FLOP 3";
		case 5: return "TURN";
		case 6: return "RIVER";
		default: return "UNKNOWN";
	}
}

// Player initialization state tracking
int32_t player_init_state = 0;

const char *player_init_state_str(int32_t state)
{
	switch (state) {
		case P_INIT_WALLET_READY: return "WALLET_READY";
		case P_INIT_TABLE_FOUND:  return "TABLE_FOUND";
		case P_INIT_WAIT_JOIN:    return "WAIT_JOIN";
		case P_INIT_JOINING:      return "JOINING";
		case P_INIT_JOINED:       return "JOINED";
		case P_INIT_DECK_READY:   return "DECK_READY";
		case P_INIT_IN_GAME:      return "IN_GAME";
		default:                  return "UNKNOWN";
	}
}

int32_t player_init_deck()
{
	int32_t retval = OK;
	char str[65], game_id_str[65];
	cJSON *cjson_player_cards = NULL, *player_deck = NULL;

	// Player ID numbering range from [0...8]
	if ((p_deck_info.player_id < 0) || (p_deck_info.player_id > 8))
		return ERR_INVALID_PLAYER_ID;

	p_deck_info.p_kp = gen_keypair();

	gen_deck(p_deck_info.player_r, CARDS_MAXCARDS);

	// Save deck info to local DB immediately after generation (for rejoin support)
	bits256_str(game_id_str, p_deck_info.game_id);
	retval = save_player_deck_info(game_id_str, player_config.table_id, p_deck_info.player_id);
	if (retval != OK) {
		dlg_error("Failed to save player deck info to local DB");
		// Continue anyway - this is non-fatal for first run
	}

	player_deck = cJSON_CreateObject();
	jaddnum(player_deck, "id", p_deck_info.player_id);
	jaddbits256(player_deck, "pubkey", p_deck_info.p_kp.prod);
	jadd(player_deck, "cardinfo", cjson_player_cards = cJSON_CreateArray());
	for (int32_t i = 0; i < CARDS_MAXCARDS; i++) {
		jaddistr(cjson_player_cards, bits256_str(str, p_deck_info.player_r[i].prod));
	}

	dlg_info("Updating %s key...", T_GAME_ID_KEY);
	cJSON *out = poker_append_key_hex(player_config.verus_pid, T_GAME_ID_KEY,
					     bits256_str(str, p_deck_info.game_id), false);
	if (!out) {
		dlg_error("Failed to update %s key after retries", T_GAME_ID_KEY);
		return ERR_GAME_STATE_UPDATE;
	}

	dlg_info("Updating %s key...", PLAYER_DECK_KEY);
	out = poker_append_key_json(
		player_config.verus_pid, get_key_data_vdxf_id(PLAYER_DECK_KEY, bits256_str(str, p_deck_info.game_id)),
		player_deck, true);
	if (!out) {
		dlg_error("Failed to update %s key after retries", PLAYER_DECK_KEY);
		return ERR_GAME_STATE_UPDATE;
	}
	DLG_JSON(info, "%s", out);

	dlg_info("Updating player game state to player_id...");
	out = append_game_state(player_config.verus_pid, G_DECK_SHUFFLING_P, NULL);
	if (!out)
		return ERR_GAME_STATE_UPDATE;
	DLG_JSON(info, "%s", out);

	return OK;
}

int32_t decode_card(bits256 b_blinded_card, bits256 blinded_value, cJSON *dealer_blind_info)
{
	int32_t card_value = -1;
	char str1[65], str2[65];
	bits256 blinded_value_inv, d_blinded_card;

	blinded_value_inv = crecip_donna(blinded_value);
	d_blinded_card = fmul_donna(blinded_value_inv, b_blinded_card);

	dlg_info("Dealer blinded card :: %s", bits256_str(str1, d_blinded_card));

	for (int32_t i = 0; i < CARDS_MAXCARDS; i++) {
		for (int32_t j = 0; j < CARDS_MAXCARDS; j++) {
			if (strcmp(bits256_str(str1, d_blinded_card),
				   bits256_str(str2, curve25519(p_deck_info.player_r[i].priv,
								jbits256i(dealer_blind_info, j)))) == 0) {
				card_value = p_deck_info.player_r[i].priv.bytes[30];
				dlg_info("card::%x\n", p_deck_info.player_r[i].priv.bytes[30]);
			}
		}
	}
	return card_value;
}

// Check if card_type is a community card (board card)
static bool is_community_card(int32_t card_type)
{
	return (card_type == flop_card_1 || card_type == flop_card_2 || card_type == flop_card_3 ||
		card_type == turn_card || card_type == river_card);
}

// Map raw card_id (from deal_next_card) to local hand position (0-6).
// This is needed because card_ids are interleaved across players and can exceed hand_size.
// Hand positions: 0=hole1, 1=hole2, 2=flop1, 3=flop2, 4=flop3, 5=turn, 6=river
static int32_t card_id_to_hand_index(int32_t card_id, int32_t player_id, int32_t nplayers, int32_t card_type)
{
	if (card_type == hole_card) {
		// Hole cards: first hole card dealt to this player → 0, second → 1
		// card_id for first hole = player_id, second = nplayers + player_id
		if (nplayers > 0 && card_id == player_id)
			return 0;
		else if (nplayers > 0 && card_id == nplayers + player_id)
			return 1;
		return -1; // Not this player's hole card
	}
	if (card_type == flop_card_1) return 2;
	if (card_type == flop_card_2) return 3;
	if (card_type == flop_card_3) return 4;
	if (card_type == turn_card)   return 5;
	if (card_type == river_card)  return 6;
	return -1;
}

// Report decoded card to player's identity (for dealer verification of community cards)
static int32_t report_decoded_card(int32_t card_id, int32_t card_type, int32_t card_value)
{
	char str[65];
	cJSON *decoded_info = NULL, *out = NULL;

	decoded_info = cJSON_CreateObject();
	cJSON_AddNumberToObject(decoded_info, "card_id", card_id);
	cJSON_AddNumberToObject(decoded_info, "card_type", card_type);
	cJSON_AddNumberToObject(decoded_info, "card_value", card_value);

	out = poker_append_key_json(player_config.verus_pid,
		get_key_data_vdxf_id(P_DECODED_CARD_KEY, bits256_str(str, p_deck_info.game_id)),
		decoded_info, true);

	if (!out) {
		dlg_error("Failed to report decoded card to player ID");
		cJSON_Delete(decoded_info);
		return ERR_UPDATEIDENTITY;
	}

	dlg_info("Reported decoded card to player ID: card_id=%d, card_type=%d, value=%d",
		card_id, card_type, card_value);
	cJSON_Delete(decoded_info);
	return OK;
}

int32_t reveal_card(char *table_id)
{
	int32_t retval = OK, player_id, card_id, card_value = -1, card_type;
	char *game_id_str = NULL, str[65];
	cJSON *game_state_info = NULL, *bv_info = NULL, *b_blinded_deck = NULL, *dealer_blind_info = NULL, *bv = NULL;
	bits256 b_blinded_card, blinded_value;

	game_state_info = get_game_state_info(table_id);
	player_id = jint(game_state_info, "player_id");
	card_id = jint(game_state_info, "card_id");
	card_type = jint(game_state_info, "card_type");

	if ((player_id == p_deck_info.player_id) || (player_id == -1)) {
		// Map raw card_id to hand position (0-6)
		int32_t hand_idx = card_id_to_hand_index(card_id, p_deck_info.player_id, num_of_players, card_type);

		// Check if we already decoded this card (from local state)
		if (hand_idx >= 0 && p_local_state.decoded_cards[hand_idx] >= 0) {
			dlg_info("Card %d (hand_idx=%d) already decoded from local state: value=%d",
				card_id, hand_idx, p_local_state.decoded_cards[hand_idx]);
			return OK;
		}

		game_id_str = poker_get_key_str(table_id, T_GAME_ID_KEY);

		// For community cards, first check if already revealed on table ID
		if (is_community_card(card_type)) {
			cJSON *board_cards = get_cJSON_from_id_key_vdxfid_from_height(table_id,
				get_key_data_vdxf_id(T_BOARD_CARDS_KEY, game_id_str), g_start_block);
			if (board_cards) {
				int32_t existing_value = -1;
				switch (card_type) {
				case flop_card_1:
					existing_value = jinti(jobj(board_cards, "flop"), 0);
					break;
				case flop_card_2:
					existing_value = jinti(jobj(board_cards, "flop"), 1);
					break;
				case flop_card_3:
					existing_value = jinti(jobj(board_cards, "flop"), 2);
					break;
				case turn_card:
					existing_value = jint(board_cards, "turn");
					break;
				case river_card:
					existing_value = jint(board_cards, "river");
					break;
				}
				if (existing_value >= 0) {
					dlg_info("Board card already revealed on table: type=%d, value=%d",
						card_type, existing_value);
					// Save to local state using hand position
					if (hand_idx >= 0) {
						update_player_decoded_card(hand_idx, existing_value);
					}
					return OK;
				}
			}
		}

		int bv_attempts = 0;
		while (1) {
			if (++bv_attempts > 100) {
				dlg_error("Timed out waiting for BV info after %d attempts", bv_attempts);
				return ERR_BV_UPDATE;
			}
			bv_info = get_cJSON_from_id_key_vdxfid_from_height(table_id,
							       get_key_data_vdxf_id(T_CARD_BV_KEY, game_id_str), g_start_block);
			if (!bv_info) {
				dlg_info("BV INFO hasn't revealed its secret yet");
				wait_for_a_blocktime();
				continue;
			}
			DLG_JSON(info, "%s", bv_info);

			bv = jobj(bv_info, "bv");
			if (!bv) {
				dlg_error("BV is missing from bv_info - cashier may not have provided blinding values");
				cJSON_Delete(bv_info);
				return ERR_BV_UPDATE;
			}
			DLG_JSON(info, "%s", bv);
			if ((jint(bv_info, "card_id") == card_id) && (jint(bv_info, "player_id") == player_id))
				break;
			dlg_info("BV is for different card (got card_id=%d player_id=%d, want card_id=%d player_id=%d) - waiting...",
				jint(bv_info, "card_id"), jint(bv_info, "player_id"), card_id, player_id);
			cJSON_Delete(bv_info);
			bv_info = NULL;
			wait_for_a_blocktime();
		}

		b_blinded_deck = get_cJSON_from_id_key_vdxfid_from_height(table_id, get_key_data_vdxf_id(all_t_b_p_keys[player_id],
											     game_id_str), g_start_block);
		b_blinded_card = jbits256i(b_blinded_deck, card_id);
		if (player_id == -1)
			blinded_value = jbits256i(bv, player_id);
		else
			blinded_value = jbits256i(bv, 0);

		dlg_info("blinded_value::%s", bits256_str(str, blinded_value));
		dlg_info("blinded_card::%s", bits256_str(str, b_blinded_card));
		dealer_blind_info =
			get_cJSON_from_id_key_vdxfid_from_height(table_id, get_key_data_vdxf_id(T_D_DECK_KEY, game_id_str), g_start_block);
		DLG_JSON(info, "dealer_blind_info::%s", dealer_blind_info);
		card_value = decode_card(b_blinded_card, blinded_value, dealer_blind_info);
		if (bv_info) { cJSON_Delete(bv_info); bv_info = NULL; }
		if (b_blinded_deck) { cJSON_Delete(b_blinded_deck); b_blinded_deck = NULL; }
		if (dealer_blind_info) { cJSON_Delete(dealer_blind_info); dealer_blind_info = NULL; }
		dlg_info("card_value ::%d", card_value);
		if (card_value == -1) {
			dlg_warn("Card decode failed for card_id=%d - will retry on next poll", card_id);
			retval = OK;  // Return OK so game loop retries
		} else {
			// ========== CARD REVEALED ==========
			dlg_info("╔════════════════════════════════════════╗");
			dlg_info("║  🃏 CARD REVEALED: %-20s ║", get_card_name(card_value));
			dlg_info("║  Type: %-10s  Card ID: %-10d ║", get_card_type_name(card_type), card_id);
			dlg_info("╚════════════════════════════════════════╝");
			
			// Save decoded card to local state using hand position mapping
			if (hand_idx >= 0) {
				update_player_decoded_card(hand_idx, card_value);
				dlg_info("Saved card_id=%d (type=%d) as hand_idx=%d with value %d to local DB",
					card_id, card_type, hand_idx, card_value);
			} else {
				dlg_warn("Cannot map card_id=%d type=%d to hand position (player_id=%d, nplayers=%d)",
					card_id, card_type, p_deck_info.player_id, num_of_players);
			}

			// Send GUI message for each card reveal
			{
				// Hole cards: send when we have both (always at hand positions 0 and 1)
				if (card_type == hole_card) {
					int32_t h1 = p_local_state.decoded_cards[0];
					int32_t h2 = p_local_state.decoded_cards[1];
					// Current card may not be in decoded_cards yet (DB write pending)
					if (hand_idx == 0) h1 = card_value;
					if (hand_idx == 1) h2 = card_value;
					if (h1 >= 0 && h2 >= 0) {
						dlg_info("Sending hole cards to GUI: %s, %s",
							get_card_name(h1), get_card_name(h2));
						cJSON *deal_msg = gui_build_deal_holecards(h1, h2, 0.0);
						gui_send_message(deal_msg);
						cJSON_Delete(deal_msg);
					}
				} else if (is_community_card(card_type)) {
					// Board cards are at hand positions 2-6
					int32_t board[5];
					int32_t board_count = 0;
					for (int i = 0; i < 5; i++) {
						board[i] = p_local_state.decoded_cards[2 + i];
					}
					// Current card may not be in decoded_cards yet
					int32_t board_pos = hand_idx - 2; // hand_idx 2-6 → board_pos 0-4
					if (board_pos >= 0 && board_pos < 5) {
						board[board_pos] = card_value;
					}
					// Count how many board cards are revealed
					for (int i = 0; i < 5; i++) {
						if (board[i] >= 0) board_count = i + 1;
					}
					cJSON *deal_msg = gui_build_deal_board(board, board_count);
					gui_send_message(deal_msg);
					cJSON_Delete(deal_msg);
				}
			}

			// For community cards, report to player ID for dealer verification
			if (is_community_card(card_type)) {
				report_decoded_card(card_id, card_type, card_value);
			}
		}
	}
	return retval;
}

static int32_t handle_player_reveal_card(char *table_id)
{
	int32_t retval = OK;
	cJSON *game_state_info = NULL, *player_game_state_info = NULL;

	player_game_state_info = get_game_state_info(player_config.verus_pid);
	game_state_info = get_game_state_info(table_id);

	if (!game_state_info) {
		dlg_warn("game_state_info is NULL - table may not have updated yet, will retry");
		return retval;  // Returns OK so caller retries on next poll
	}
	if (jint(game_state_info, "player_id") != p_deck_info.player_id) {
		// Not this players turn
		dlg_info("Not this players turn...");
		return retval;
	}
	if ((!player_game_state_info) || (jint(game_state_info, "card_id") > jint(player_game_state_info, "card_id"))) {
		retval = reveal_card(table_id);
		if (retval == OK) {
			append_game_state(player_config.verus_pid, G_REVEAL_CARD_P_DONE, game_state_info);
			DLG_JSON(info, "Updating player's revealed card info :: %s", game_state_info);
		}
	}
	return retval;
}

/**
 * Read betting state from table ID
 */
cJSON *player_read_betting_state(char *table_id)
{
	char *game_id_str = poker_get_key_str(table_id, T_GAME_ID_KEY);
	if (!game_id_str) return NULL;
	
	return get_cJSON_from_id_key_vdxfid_from_height(
		table_id,
		get_key_data_vdxf_id(T_BETTING_STATE_KEY, game_id_str),
		g_start_block);
}

/**
 * Write betting action to player ID
 * Amount is in CHIPS (e.g., 0.01, 0.02)
 */
int32_t player_write_betting_action(char *table_id, const char *action, double amount)
{
	int32_t retval = OK;
	char game_id_str[65];
	cJSON *action_obj = NULL, *out = NULL;
	
	bits256_str(game_id_str, p_deck_info.game_id);
	
	action_obj = cJSON_CreateObject();
	cJSON_AddStringToObject(action_obj, "action", action);
	cJSON_AddNumberToObject(action_obj, "amount", amount);  // In CHIPS
	cJSON_AddNumberToObject(action_obj, "round", p_local_state.last_game_state);  // Use as round tracking
	
	dlg_info("Writing betting action: %s, amount=%.4f CHIPS", action, amount);
	
	out = poker_update_key_json(player_config.verus_pid,
		get_key_data_vdxf_id(P_BETTING_ACTION_KEY, game_id_str),
		action_obj, true);
	
	cJSON_Delete(action_obj);
	
	if (!out) {
		return ERR_GAME_STATE_UPDATE;
	}
	
	return retval;
}

/**
 * Display betting options and get player input
 */
int32_t player_handle_betting(char *table_id)
{
	int32_t retval = OK;
	cJSON *betting_state = NULL;
	
	betting_state = player_read_betting_state(table_id);
	if (!betting_state) {
		return OK;  // No betting state yet, wait
	}
	
	int32_t current_turn = jint(betting_state, "current_turn");
	const char *action = jstr(betting_state, "action");
	int32_t round = jint(betting_state, "round");
	double pot = jdouble(betting_state, "pot");           // In CHIPS
	double min_amount = jdouble(betting_state, "min_amount");  // In CHIPS
	
	// Check if it's our turn
	if (current_turn != p_deck_info.player_id) {
		// Not our turn, just display status
		dlg_info("Waiting for Player %d to act (pot: %.4f CHIPS)", current_turn, pot);
		return OK;
	}
	
	// Calculate remaining time
	int64_t turn_start_time = (int64_t)jdouble(betting_state, "turn_start_time");
	int32_t timeout_secs = jint(betting_state, "timeout_secs");
	int64_t current_time = (int64_t)time(NULL);
	int64_t elapsed = current_time - turn_start_time;
	int64_t remaining = timeout_secs - elapsed;
	if (remaining < 0) remaining = 0;
	
	// It's our turn!
	dlg_info("═══════════════════════════════════════════");
	dlg_info("  🎯 YOUR TURN - Player %d (%s)             ", p_deck_info.player_id, player_config.verus_pid);
	dlg_info("  ⏰ Time remaining: %lld seconds           ", (long long)remaining);
	dlg_info("═══════════════════════════════════════════");
	dlg_info("  Action: %s", action);
	dlg_info("  Round: %d, Pot: %.4f CHIPS", round, pot);
	dlg_info("  Minimum to call: %.4f CHIPS", min_amount);
	
	// Display possibilities
	cJSON *possibilities = cJSON_GetObjectItem(betting_state, "possibilities");
	if (possibilities) {
		printf("\n  Options: ");
		for (int i = 0; i < cJSON_GetArraySize(possibilities); i++) {
			cJSON *opt = cJSON_GetArrayItem(possibilities, i);
			if (opt && opt->valuestring) {
				printf("[%s] ", opt->valuestring);
			}
		}
		printf("\n");
	}
	
	// Display player funds
	cJSON *player_funds = cJSON_GetObjectItem(betting_state, "player_funds");
	if (player_funds && p_deck_info.player_id < cJSON_GetArraySize(player_funds)) {
		cJSON *funds_item = cJSON_GetArrayItem(player_funds, p_deck_info.player_id);
		if (funds_item) {
			double my_funds = funds_item->valuedouble;
			dlg_info("  Your funds: %.4f CHIPS", my_funds);
		}
	}
	
	dlg_info("═══════════════════════════════════════════");
	
	// Handle based on betting mode
	extern int g_betting_mode;
	
	// Send GUI message for betting round (always log for testing)
	{
		// Build possibilities array for GUI
		int32_t poss_arr[8] = {0, 1, 2, 3, 4, 5, 6, 7};  // fold, call, raise, check, allin, bet
		int32_t poss_count = 6;
		
		// Build player funds array
		double funds_arr[CARDS_MAXPLAYERS];
		cJSON *player_funds_json = cJSON_GetObjectItem(betting_state, "player_funds");
		int32_t num_players = player_funds_json ? cJSON_GetArraySize(player_funds_json) : 0;
		for (int i = 0; i < num_players && i < CARDS_MAXPLAYERS; i++) {
			cJSON *fi = cJSON_GetArrayItem(player_funds_json, i);
			funds_arr[i] = fi ? fi->valuedouble : 0.0;
		}
		
		// Calculate min raise (typically 2x the call amount or big blind)
		double min_raise = min_amount > 0 ? min_amount * 2 : 0.02;
		
		cJSON *gui_msg = gui_build_betting_round(
			p_deck_info.player_id,
			pot,
			min_amount,
			min_raise,
			poss_arr,
			poss_count,
			funds_arr,
			num_players
		);
		gui_send_message(gui_msg);
		cJSON_Delete(gui_msg);

		// In GUI mode, wait for WebSocket message from GUI
		if (g_betting_mode == BET_MODE_GUI) {
			// Store context for the WebSocket GUI handler in client.c
			snprintf(g_gui_betting_action, sizeof(g_gui_betting_action), "%s", action);
			g_gui_betting_min_amount = min_amount;
			dlg_info("Waiting for GUI action...");
			return OK;
		}
	}
	
	if (g_betting_mode == BET_MODE_CLI) {
		// CLI mode - get input from user
		char input[256] = {0};
		double bet_amount = 0.0;
		
		// For blinds, auto-post (mandatory)
		if (strcmp(action, "small_blind") == 0 || strcmp(action, "big_blind") == 0) {
			printf("\n  [AUTO] Posting %s: %.4f CHIPS\n", action, min_amount);
			retval = player_write_betting_action(table_id, "bet", min_amount);
		} else {
			// Regular betting round - two-step input
			// Step 1: Get action
			printf("\n  Enter action (fold/check/call/raise/allin): ");
			fflush(stdout);
			
			if (fgets(input, sizeof(input), stdin) != NULL) {
				// Remove newline
				input[strcspn(input, "\n")] = 0;
				
				// Convert to lowercase for easier comparison
				for (int i = 0; input[i]; i++) {
					if (input[i] >= 'A' && input[i] <= 'Z') input[i] += 32;
				}
				
				if (strcmp(input, "fold") == 0 || strcmp(input, "f") == 0) {
					printf("  → Folding...\n");
					retval = player_write_betting_action(table_id, "fold", 0.0);
					
				} else if (strcmp(input, "check") == 0 || strcmp(input, "x") == 0) {
					if (min_amount > 0.0) {
						printf("  ⚠ Cannot check, must call %.4f CHIPS or fold\n", min_amount);
						printf("  Enter action (call/fold): ");
						fflush(stdout);
						if (fgets(input, sizeof(input), stdin) != NULL) {
							input[strcspn(input, "\n")] = 0;
							if (strcmp(input, "call") == 0 || strcmp(input, "c") == 0) {
								printf("  → Calling %.4f CHIPS...\n", min_amount);
								retval = player_write_betting_action(table_id, "call", min_amount);
							} else {
								printf("  → Folding...\n");
								retval = player_write_betting_action(table_id, "fold", 0.0);
							}
						}
					} else {
						printf("  → Checking...\n");
						retval = player_write_betting_action(table_id, "check", 0.0);
					}
					
				} else if (strcmp(input, "call") == 0 || strcmp(input, "c") == 0) {
					printf("  → Calling %.4f CHIPS...\n", min_amount);
					retval = player_write_betting_action(table_id, "call", min_amount);
					
				} else if (strcmp(input, "raise") == 0 || strcmp(input, "r") == 0) {
					// Step 2: Get raise amount
					double raise_amount = 0.0;
					printf("  Enter raise amount (min %.4f): ", min_amount * 2);
					fflush(stdout);
					if (fgets(input, sizeof(input), stdin) != NULL) {
						input[strcspn(input, "\n")] = 0;
						raise_amount = atof(input);
						if (raise_amount <= 0.0) {
							raise_amount = min_amount * 2;  // Default to min raise
						}
					}
					printf("  → Raising to %.4f CHIPS...\n", raise_amount);
					retval = player_write_betting_action(table_id, "raise", raise_amount);
					
				} else if (strcmp(input, "bet") == 0 || strcmp(input, "b") == 0) {
					// Step 2: Get bet amount
					double bet_amt = 0.0;
					printf("  Enter bet amount: ");
					fflush(stdout);
					if (fgets(input, sizeof(input), stdin) != NULL) {
						input[strcspn(input, "\n")] = 0;
						bet_amt = atof(input);
						if (bet_amt <= 0.0) {
							bet_amt = min_amount > 0.0 ? min_amount : 0.01;
						}
					}
					printf("  → Betting %.4f CHIPS...\n", bet_amt);
					retval = player_write_betting_action(table_id, "bet", bet_amt);
					
				} else if (strcmp(input, "allin") == 0 || strcmp(input, "a") == 0) {
					// Get player funds and go all-in
					cJSON *pf = cJSON_GetObjectItem(betting_state, "player_funds");
					double my_funds = 0.0;
					if (pf && p_deck_info.player_id < cJSON_GetArraySize(pf)) {
						cJSON *my_item = cJSON_GetArrayItem(pf, p_deck_info.player_id);
						my_funds = my_item ? my_item->valuedouble : 0.0;
					}
					printf("  → Going ALL-IN with %.4f CHIPS!\n", my_funds);
					retval = player_write_betting_action(table_id, "allin", my_funds);
					
				} else if (strlen(input) == 0) {
					// Empty input - auto check/call
					if (min_amount > 0.0) {
						printf("  → Auto-calling %.4f CHIPS...\n", min_amount);
						retval = player_write_betting_action(table_id, "call", min_amount);
					} else {
						printf("  → Auto-checking...\n");
						retval = player_write_betting_action(table_id, "check", 0.0);
					}
					
				} else {
					printf("  ⚠ Unknown action '%s'.\n", input);
					printf("  Valid actions: fold, check, call, raise, bet, allin\n");
					// Don't send anything, let timeout handle it or user can retry
					return OK;
				}
			}
		}
		p_local_state.last_game_state = round;
	} else {
		// AUTO mode - auto-respond based on action type
		if (strcmp(action, "small_blind") == 0) {
			dlg_info("Posting small blind: %.4f CHIPS", min_amount);
			retval = player_write_betting_action(table_id, "bet", min_amount);
			p_local_state.last_game_state = round;
		} else if (strcmp(action, "big_blind") == 0) {
			dlg_info("Posting big blind: %.4f CHIPS", min_amount);
			retval = player_write_betting_action(table_id, "bet", min_amount);
			p_local_state.last_game_state = round;
		} else {
			// Regular betting - for testing, auto-call or check
			if (min_amount > 0.0) {
				dlg_info("Auto-calling %.4f CHIPS...", min_amount);
				retval = player_write_betting_action(table_id, "call", min_amount);
			} else {
				dlg_info("Auto-checking...");
				retval = player_write_betting_action(table_id, "check", 0.0);
			}
			p_local_state.last_game_state = round;
		}
	}
	
	return retval;
}

int32_t handle_game_state_player(char *table_id)
{
	int32_t game_state, retval = OK;
	static int32_t last_logged_state = -1;

	game_state = get_game_state(table_id);
	if (game_state != last_logged_state) {
		dlg_info("%s", game_state_str(game_state));
		last_logged_state = game_state;
	}
	switch (game_state) {
	case G_REVEAL_CARD:
		retval = handle_player_reveal_card(table_id);
		break;
	case G_ROUND_BETTING:
		retval = player_handle_betting(table_id);
		break;
	case G_SHOWDOWN: {
		// Only reveal hole cards once per game
		static bits256 last_revealed_game = { 0 };
		if (bits256_nonz(last_revealed_game) &&
		    bits256_cmp(last_revealed_game, p_deck_info.game_id) == 0) {
			break;
		}
		dlg_info("SHOWDOWN - Revealing hole cards (hand positions: 0=hole1, 1=hole2, 2-6=board)");
		for (int i = 0; i < hand_size; i++) {
			dlg_info("  decoded_cards[%d] = %d %s", i, p_local_state.decoded_cards[i],
				p_local_state.decoded_cards[i] >= 0 ? get_card_name(p_local_state.decoded_cards[i]) : "(empty)");
		}
		// With hand position mapping: hole cards are always at positions 0 and 1
		int32_t card1 = p_local_state.decoded_cards[0];
		int32_t card2 = p_local_state.decoded_cards[1];
		if (card1 >= 0 && card2 >= 0) {
			char game_id_str[65];
			bits256_str(game_id_str, p_deck_info.game_id);
			cJSON *holecards = cJSON_CreateObject();
			cJSON_AddNumberToObject(holecards, "card1", card1);
			cJSON_AddNumberToObject(holecards, "card2", card2);
			// Include board cards so dealer can read them at showdown
			cJSON *board_arr = cJSON_CreateArray();
			for (int32_t b = 2; b < hand_size; b++) {
				cJSON_AddItemToArray(board_arr, cJSON_CreateNumber(p_local_state.decoded_cards[b]));
			}
			cJSON_AddItemToObject(holecards, "board", board_arr);
			cJSON *out = poker_update_key_json(player_config.verus_pid,
				get_key_data_vdxf_id(P_REVEALED_HOLECARDS_KEY, game_id_str),
				holecards, true);
			cJSON_Delete(holecards);
			if (out) {
				dlg_info("Revealed hole cards: %s, %s",
					get_card_name(card1), get_card_name(card2));
				last_revealed_game = p_deck_info.game_id;
			} else {
				dlg_error("Failed to reveal hole cards to blockchain");
			}
		} else {
			dlg_warn("Cannot reveal hole cards - not decoded (card1=%d, card2=%d)", card1, card2);
		}
		break;
	}
	case G_SETTLEMENT_PENDING: {
		// Read showdown results from blockchain and send to GUI
		static bits256 last_result_game = { 0 };
		if (bits256_nonz(last_result_game) &&
		    bits256_cmp(last_result_game, p_deck_info.game_id) == 0) {
			break;
		}
		char gid_str[65];
		bits256_str(gid_str, p_deck_info.game_id);
		cJSON *result = get_cJSON_from_id_key_vdxfid_from_height(
			player_config.table_id,
			get_key_data_vdxf_id(T_SHOWDOWN_RESULT_KEY, gid_str),
			g_start_block);
		if (result) {
			cJSON *winners_arr = cJSON_GetObjectItem(result, "winners");
			cJSON *win_amounts = cJSON_GetObjectItem(result, "win_amounts");
			cJSON *hc_arr = cJSON_GetObjectItem(result, "holecards");
			cJSON *board_arr = cJSON_GetObjectItem(result, "board");

			if (winners_arr && hc_arr && board_arr) {
				int32_t winner_count = cJSON_GetArraySize(winners_arr);
				int32_t winner_list[CARDS_MAXPLAYERS];
				double total_win = 0;
				for (int32_t i = 0; i < winner_count && i < CARDS_MAXPLAYERS; i++) {
					winner_list[i] = jinti(winners_arr, i);
					if (win_amounts)
						total_win += jdoublei(win_amounts, i);
				}

				int32_t np = cJSON_GetArraySize(hc_arr);
				int32_t hc_data[CARDS_MAXPLAYERS][2];
				int32_t *hc_ptrs[CARDS_MAXPLAYERS];
				for (int32_t i = 0; i < np && i < CARDS_MAXPLAYERS; i++) {
					cJSON *pc = cJSON_GetArrayItem(hc_arr, i);
					hc_data[i][0] = jinti(pc, 0);
					hc_data[i][1] = jinti(pc, 1);
					hc_ptrs[i] = hc_data[i];
				}

				int32_t board[5] = {-1, -1, -1, -1, -1};
				for (int32_t i = 0; i < 5 && i < cJSON_GetArraySize(board_arr); i++)
					board[i] = jinti(board_arr, i);

				cJSON *final_msg = gui_build_final_info(
					winner_list, winner_count, total_win,
					hc_ptrs, board, np);
				gui_send_message(final_msg);
				cJSON_Delete(final_msg);

				dlg_info("Showdown results sent to GUI");
				last_result_game = p_deck_info.game_id;
			}
		}
		break;
	}
	case G_SETTLEMENT_COMPLETE:
		dlg_info("PAYOUT RECEIVED - Game Finished!");
		break;
	}
	return retval;
}

int32_t handle_verus_player()
{
	int32_t retval = OK;
	char game_id_str[65];

	// Initialize local state
	srand(time(NULL) ^ getpid());
	init_p_local_state();

	// Check if poker is ready
	if ((retval = verify_poker_setup()) != OK) {
		dlg_error("Poker not ready: %s", bet_err_str(retval));
		return retval;
	}
	// Parse Verus player configuration
	if ((retval = bet_parse_verus_player()) != OK) {
		dlg_error("Failed to parse Verus player configuration: %s", bet_err_str(retval));
		return retval;
	}
	
	// State 1: Wallet + ID ready, can read blockchain
	send_init_state_to_gui(P_INIT_WALLET_READY);
	dlg_info("Player init state: %s", player_init_state_str(P_INIT_WALLET_READY));

	// In GUI mode, wait for GUI to request table info before trying to find a table
	if (g_betting_mode == BET_MODE_GUI) {
		dlg_info("GUI mode: waiting for GUI to send table_info request...");
		pthread_mutex_lock(&gui_table_mutex);
		while (gui_table_requested == 0) {
			pthread_cond_wait(&gui_table_cond, &gui_table_mutex);
		}
		// Reset for next request
		gui_table_requested = 0;
		pthread_mutex_unlock(&gui_table_mutex);
		dlg_info("GUI triggered table finding");
	}

	// Find a table
	bool already_joined = false;
	if ((retval = poker_find_table()) != OK) {
		if (retval == ERR_DUPLICATE_PLAYERID) {
			dlg_info("Player already exists in the table. Skipping join, proceeding to deck shuffling.");
			already_joined = true;
			retval = OK;  // Clear the error since we're handling it
		} else {
			dlg_error("Failed to find table: %s", bet_err_str(retval));
			// In GUI mode, send error to GUI instead of returning
			if (g_betting_mode == BET_MODE_GUI) {
				cJSON *error_msg = cJSON_CreateObject();
				cJSON_AddStringToObject(error_msg, "method", "error");
				cJSON_AddStringToObject(error_msg, "error", bet_err_str(retval));
				cJSON_AddStringToObject(error_msg, "message", "Failed to find table");
				player_lws_write(error_msg);
				cJSON_Delete(error_msg);
			}
			return retval;
		}
	}
	dlg_info("Table found");
	print_struct_table(&player_t);
	
	// State 2: Found table, have table info
	send_init_state_to_gui(P_INIT_TABLE_FOUND);
	dlg_info("Player init state: %s", player_init_state_str(P_INIT_TABLE_FOUND));
	
	// If GUI mode, wait for GUI to approve join
	if (g_betting_mode == BET_MODE_GUI) {
		// State 3: Waiting for GUI approval
		send_init_state_to_gui(P_INIT_WAIT_JOIN);
		dlg_info("Player init state: %s - waiting for GUI approval...", player_init_state_str(P_INIT_WAIT_JOIN));
		
		// Wait for GUI to send join_table message, refreshing table info every 5 seconds
		extern void bet_player_table_info(void);
		struct timespec ts;
		
		pthread_mutex_lock(&gui_join_mutex);
		dlg_info("Waiting for GUI join signal (gui_join_approved=%d)...", gui_join_approved);
		while (gui_join_approved == 0) {
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 5;  // 5 second timeout
			
			int wait_result = pthread_cond_timedwait(&gui_join_cond, &gui_join_mutex, &ts);
			if (wait_result == ETIMEDOUT && gui_join_approved == 0) {
				// Timeout - refresh table info for GUI
				pthread_mutex_unlock(&gui_join_mutex);
				bet_player_table_info();
				pthread_mutex_lock(&gui_join_mutex);
			}
		}
		// Reset for next join
		gui_join_approved = 0;
		pthread_mutex_unlock(&gui_join_mutex);
		
		dlg_info("GUI join approved, proceeding to join table");
	}

	// Join the table (skip if already joined)
	if (!already_joined) {
		// State 4: Joining table (executing payin_tx)
		send_init_state_to_gui(P_INIT_JOINING);
		dlg_info("Player init state: %s", player_init_state_str(P_INIT_JOINING));
		
		if ((retval = poker_join_table()) != OK) {
			dlg_error("Failed to join table: %s", bet_err_str(retval));
			return retval;
		}
		dlg_info("Table Joined");
		
		// State 5: Successfully joined (have seat)
		send_init_state_to_gui(P_INIT_JOINED);
		dlg_info("Player init state: %s", player_init_state_str(P_INIT_JOINED));
	}

	// In GUI mode, wait for all players to join (G_PLAYERS_JOINED state)
	// Periodically send table_info updates so GUI can show who else has joined
	if (g_betting_mode == BET_MODE_GUI) {
		extern void bet_player_table_info(void);
		int32_t game_state;
		
		dlg_info("Waiting for all players to join...");
		while ((game_state = get_game_state(player_config.table_id)) < G_PLAYERS_JOINED) {
			bet_player_table_info();  // Send updated table info to GUI
			sleep(5);  // Check every 5 seconds
		}
		dlg_info("All players joined, game state: %s", game_state_str(game_state));
	}

	// Get player ID
	if ((retval = get_player_id(&p_deck_info.player_id)) != OK) {
		dlg_error("Failed to get player ID: %s", bet_err_str(retval));
		return retval;
	}
	dlg_info("Player ID: %d", p_deck_info.player_id);

	// Get the current game_id from the table
	char *game_id_from_table = poker_get_key_str(player_config.table_id, T_GAME_ID_KEY);
	if (game_id_from_table) {
		p_deck_info.game_id = bits256_conv(game_id_from_table);
		strncpy(game_id_str, game_id_from_table, sizeof(game_id_str) - 1);
		game_id_str[sizeof(game_id_str) - 1] = '\0';
		dlg_info("Game ID from table: %s", game_id_str);
	} else {
		// No game_id yet - will be set during deck init
		memset(game_id_str, 0, sizeof(game_id_str));
	}

	// Check if game is already past deck shuffling phase
	int32_t current_game_state = get_game_state(player_config.table_id);
	if (current_game_state > G_DECK_SHUFFLING_B) {
		// Game is already in progress - try to load state from local DB
		dlg_info("Game is in progress (state: %s). Attempting to load from local DB...",
			 game_state_str(current_game_state));
		
		if (strlen(game_id_str) > 0) {
			// Load deck info
			retval = load_player_deck_info(game_id_str);
			if (retval != OK) {
				dlg_error("Failed to load deck info from local DB: %s", bet_err_str(retval));
				dlg_error("Cannot rejoin - deck keys not found. Please wait for a new game.");
				return ERR_GAME_ALREADY_STARTED;
			}
			dlg_info("Successfully loaded player deck info from local DB!");

			// Load local state (payin_tx, decoded cards)
			retval = load_player_local_state(game_id_str, p_deck_info.player_id);
			if (retval == OK) {
				dlg_info("Loaded local state: payin_tx=%s, cards_decoded=%d", 
					p_local_state.payin_tx, p_local_state.cards_decoded_count);
				
				// Check if payin_tx is still unspent (game still active)
				if (strlen(p_local_state.payin_tx) > 0) {
					dlg_info("Payin TX: %s - can be used for dispute if needed", p_local_state.payin_tx);
				}
			} else {
				dlg_info("No local state found, starting fresh tracking");
				// Initialize local state for this game
				strncpy(p_local_state.game_id, game_id_str, sizeof(p_local_state.game_id) - 1);
				strncpy(p_local_state.table_id, player_config.table_id, sizeof(p_local_state.table_id) - 1);
				p_local_state.player_id = p_deck_info.player_id;
			}

			dlg_info("Rejoining game with player_id=%d", p_deck_info.player_id);
		} else {
			dlg_error("No game_id found on table. Cannot rejoin.");
			return ERR_GAME_ALREADY_STARTED;
		}
	} else if (current_game_state >= G_PLAYERS_JOINED && current_game_state <= G_DECK_SHUFFLING_B) {
		// Game is in deck shuffling phase - check if we have local deck info
		if (already_joined && strlen(game_id_str) > 0) {
			retval = load_player_deck_info(game_id_str);
			if (retval == OK) {
				dlg_info("Loaded existing deck info for this game from local DB");
				// Also load local state
				load_player_local_state(game_id_str, p_deck_info.player_id);
				// Re-write game state to blockchain so dealer can detect us
				append_game_state(player_config.verus_pid, G_DECK_SHUFFLING_P, NULL);
			} else {
				// No local deck info - need to initialize
				dlg_info("No saved deck info found, initializing new deck...");
				if ((retval = player_init_deck()) != OK) {
					dlg_error("Failed to initialize player deck: %s", bet_err_str(retval));
					return retval;
				}
				// Initialize and save local state
				strncpy(p_local_state.game_id, game_id_str, sizeof(p_local_state.game_id) - 1);
				strncpy(p_local_state.table_id, player_config.table_id, sizeof(p_local_state.table_id) - 1);
				p_local_state.player_id = p_deck_info.player_id;
				save_player_local_state(player_config.txid);  // Save with payin_tx
				dlg_info("Player deck shuffling info updated to table");
			}
		} else {
			// First join - initialize deck
			if ((retval = player_init_deck()) != OK) {
				dlg_error("Failed to initialize player deck: %s", bet_err_str(retval));
				return retval;
			}
			// Initialize and save local state with payin_tx
			strncpy(p_local_state.game_id, game_id_str, sizeof(p_local_state.game_id) - 1);
			strncpy(p_local_state.table_id, player_config.table_id, sizeof(p_local_state.table_id) - 1);
			p_local_state.player_id = p_deck_info.player_id;
			save_player_local_state(player_config.txid);
			dlg_info("Player deck shuffling info updated to table");
		}
	} else {
		// Game not started yet or waiting for players
		if ((retval = player_init_deck()) != OK) {
			dlg_error("Failed to initialize player deck: %s", bet_err_str(retval));
			return retval;
		}
		// Initialize and save local state with payin_tx
		bits256_str(game_id_str, p_deck_info.game_id);
		strncpy(p_local_state.game_id, game_id_str, sizeof(p_local_state.game_id) - 1);
		strncpy(p_local_state.table_id, player_config.table_id, sizeof(p_local_state.table_id) - 1);
		p_local_state.player_id = p_deck_info.player_id;
		save_player_local_state(player_config.txid);
		dlg_info("Player deck shuffling info updated to table");
	}
	
	// State 6: Deck initialized/loaded, ready for game
	send_init_state_to_gui(P_INIT_DECK_READY);
	dlg_info("Player init state: %s", player_init_state_str(P_INIT_DECK_READY));

	// State 7: Entering game loop
	// Re-read num_of_players now that all players have joined.
	// get_player_id() may have been called when only 1 player existed.
	{
		char *gid_str = poker_get_key_str(player_config.table_id, T_GAME_ID_KEY);
		if (gid_str) {
			cJSON *t_pi = get_cJSON_from_id_key_vdxfid_from_height(player_config.table_id,
				get_key_data_vdxf_id(T_PLAYER_INFO_KEY, gid_str), g_start_block);
			if (t_pi) {
				cJSON *pi = jobj(t_pi, "player_info");
				if (pi) {
					int32_t np = cJSON_GetArraySize(pi);
					if (np > num_of_players) {
						dlg_info("Updated num_of_players: %d -> %d", num_of_players, np);
						num_of_players = np;
					}
				}
			}
		}
	}
	send_init_state_to_gui(P_INIT_IN_GAME);
	dlg_info("Player init state: %s - entering game loop (num_of_players=%d)", player_init_state_str(P_INIT_IN_GAME), num_of_players);

	// Main game loop
	while (1) {
		retval = handle_game_state_player(player_config.table_id);
		if (retval != OK) {
			dlg_warn("Transient error in game state handling: %s (will retry)", bet_err_str(retval));
			// Don't exit on transient errors — card decode can fail on first attempt
			// and succeed on retry when blockchain data becomes available
		}
		sleep(2);
	}

	return retval;
}
