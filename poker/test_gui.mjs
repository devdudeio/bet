#!/usr/bin/env node
/**
 * Simulated GUI client for Pangea Poker
 * Connects via WebSocket to player backend (port 9001) and plays automatically.
 * Tests the same code path as the React frontend.
 */

const WS_URL = process.argv[2] || 'ws://127.0.0.1:9001';

let ws;
let joined = false;
let lastBettingAction = null;  // Track to avoid spam responses

function log(msg) {
  console.log(`[GUI-TEST ${new Date().toISOString().slice(11,19)}] ${msg}`);
}

function send(obj) {
  const msg = JSON.stringify(obj);
  log(`→ SEND: ${msg.slice(0, 300)}`);
  ws.send(msg);
}

function connect() {
  log(`Connecting to ${WS_URL}...`);
  ws = new WebSocket(WS_URL);

  ws.addEventListener('open', () => {
    log('Connected! Requesting backend status...');
    send({ method: 'backend_status' });
  });

  ws.addEventListener('message', (event) => {
    let data;
    try {
      data = JSON.parse(event.data);
    } catch (e) {
      log(`← RAW: ${event.data.slice(0, 200)}`);
      return;
    }

    const method = data.method || '?';

    switch (method) {
      case 'backend_status':
        log(`← Backend status: ${data.backend_status === 1 ? 'READY' : 'NOT READY'}`);
        if (data.backend_status === 1 && !joined) {
          log('Requesting table info...');
          send({ method: 'table_info' });
        }
        break;

      case 'table_info':
        log(`← Table info: ${data.table_id || 'unknown'}, players: ${data.num_players || 0}/${data.max_players || '?'}`);
        if (!joined) {
          log('Requesting to join table...');
          send({ method: 'join_table' });
          joined = true;
        }
        break;

      case 'join_res':
      case 'join_table':
      case 'join_ack':
        log(`← Join response: ${JSON.stringify(data).slice(0, 200)}`);
        break;

      case 'player_init_state':
        log(`← State: ${data.state_name} (${data.state})`);
        break;

      case 'deal':
        log(`← DEAL: ${JSON.stringify(data.deal)}`);
        break;

      case 'betting': {
        const action = data.action;
        const playerid = data.playerid;
        const pot = data.pot;
        const toCall = data.toCall;
        const minRaise = data.minRaiseTo;
        const possibilities = data.possibilities;
        const funds = data.player_funds;

        // Create a signature for this betting prompt to detect duplicates
        const sig = `${action}-${playerid}-${pot}-${toCall}`;
        if (sig === lastBettingAction) {
          // Already responded to this exact prompt, skip
          return;
        }

        log(`← BETTING: action=${action}, player=${playerid}, pot=${pot}, toCall=${toCall}, minRaise=${minRaise}`);
        log(`  funds: [${funds}], possibilities: [${possibilities}]`);

        if (action === 'round_betting') {
          lastBettingAction = sig;

          let chosenAction, betAmount;

          if (toCall <= 0 || toCall === undefined) {
            // Can check
            chosenAction = 3; // check
            betAmount = 0;
            log('  → Decision: CHECK');
          } else {
            // Must call or fold
            chosenAction = 5; // call
            betAmount = toCall;
            log(`  → Decision: CALL ${toCall}`);
          }

          const response = {
            method: 'betting',
            action: 'round_betting',
            playerid: playerid,
            pot: pot,
            toCall: toCall,
            minRaiseTo: minRaise,
            possibilities: [chosenAction],
            player_funds: funds,
            bet_amount: betAmount
          };

          setTimeout(() => send(response), 500);
        }
        break;
      }

      case 'showInfo':
        log(`← SHOWDOWN: ${JSON.stringify(data).slice(0, 400)}`);
        break;

      case 'winners':
        log(`← WINNERS: ${JSON.stringify(data).slice(0, 300)}`);
        break;

      case 'game_state':
        log(`← Game state: ${JSON.stringify(data).slice(0, 200)}`);
        break;

      default:
        log(`← ${method}: ${JSON.stringify(data).slice(0, 200)}`);
        break;
    }
  });

  ws.addEventListener('close', (event) => {
    log(`WebSocket closed (code=${event.code}). Reconnecting in 3s...`);
    setTimeout(connect, 3000);
  });

  ws.addEventListener('error', (event) => {
    log(`WebSocket error: ${event.message || 'unknown'}`);
  });
}

connect();

// Keep alive
setInterval(() => {}, 10000);

process.on('SIGINT', () => {
  log('Shutting down...');
  if (ws) ws.close();
  process.exit(0);
});
