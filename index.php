<?php
// SMART CAMPUS - ONE-FILE DASHBOARD. Upload only this file as index.php in your team folder.
// Readings are saved in data/smart.txt (one reading per line). The data folder is created automatically.
date_default_timezone_set('Asia/Kolkata');
$API_KEY = 'Campus123';   // same as API_KEY in the master .ino

// data folder (falls back to the server temp folder if it cannot be created)
$dir = __DIR__ . '/data';
if (!is_dir($dir)) { @mkdir($dir, 0777, true); }
$where = 'data/smart.txt';
if (!is_dir($dir) || !is_writable($dir)) {
    $dir = sys_get_temp_dir() . '/sc_' . md5(__DIR__);
    @mkdir($dir, 0777, true);
    $where = 'server temp folder (data folder is not writable)';
}
$dataFile = "$dir/smart.txt";   // sensor readings
$ledFile  = "$dir/leds.txt";    // LED commands

function read_rows($f) {
    if (!is_file($f)) return [];
    $rows = [];
    foreach (file($f, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $line) {
        $r = json_decode($line, true);
        if (is_array($r)) $rows[] = $r;
    }
    return $rows;
}
function add_row($f, $row) {            // keeps only the last 50 readings
    $rows = read_rows($f);
    $rows[] = $row;
    $rows = array_slice($rows, -50);
    $text = '';
    foreach ($rows as $r) { $text .= json_encode($r) . "\n"; }
    file_put_contents($f, $text, LOCK_EX);
}
function load($f, $def) {
    if (!file_exists($f)) return $def;
    $d = json_decode(file_get_contents($f), true);
    return is_array($d) ? $d : $def;
}
function save($f, $d) { file_put_contents($f, json_encode($d), LOCK_EX); }

function api_key() {
    if (isset($_SERVER['HTTP_X_API_KEY'])) return $_SERVER['HTTP_X_API_KEY'];
    if (function_exists('getallheaders')) {
        foreach (getallheaders() as $k => $v) if (strtolower($k) === 'x-api-key') return $v;
    }
    return $_GET['key'] ?? '';
}

// LED modes: 0=auto 1=on 2=blink 3=off
$led = load($ledFile, ['green_led' => 0, 'red_led' => 3]);
$api = $_GET['api'] ?? '';

// 1. master uploads sensor data
if ($api === 'insert') {
    if (!hash_equals($API_KEY, api_key())) { http_response_code(403); exit('Wrong API key'); }
    $d = json_decode(file_get_contents('php://input'), true);
    if (!is_array($d)) { http_response_code(400); exit('Bad JSON'); }
    $d['updated'] = time();
    add_row($dataFile, $d);
    exit('OK');
}

// 2. master asks what the LEDs should do
if ($api === 'leds') {
    header('Content-Type: application/json');
    header('Cache-Control: no-store');
    echo json_encode(['green_led' => (int)$led['green_led'], 'red_led' => (int)$led['red_led']]);
    exit;
}

// 3. master reports a button press
if ($api === 'setled') {
    if (!hash_equals($API_KEY, api_key())) { http_response_code(403); exit('Wrong API key'); }
    $which = $_GET['led'] ?? '';
    $state = (int)($_GET['state'] ?? -1);
    if (($which === 'red' || $which === 'green') && $state >= 0 && $state <= 3) {
        $led[$which . '_led'] = $state;
        save($ledFile, $led);
        exit('OK');
    }
    http_response_code(400);
    exit('Bad request');
}

// 4. button pressed on this web page
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $cmd = $_POST['command'] ?? '';
    if ($cmd === 'ON')  $led['red_led'] = 1;
    if ($cmd === 'OFF') $led['red_led'] = 3;
    save($ledFile, $led);
    header('Location: index.php');
    exit;
}

// 5. show the page
$rows   = read_rows($dataFile);
$s      = $rows ? end($rows) : [];
$recent = array_reverse(array_slice($rows, -10));       // newest first
$age    = isset($s['updated']) ? time() - $s['updated'] : -1;
$names  = [0 => 'AUTO', 1 => 'ON', 2 => 'BLINK', 3 => 'OFF'];
$ledName = $names[(int)$led['red_led']] ?? '?';

function v($a, $k, $u) {
    return (isset($a[$k]) && is_numeric($a[$k])) ? round((float)$a[$k], 1) . " $u" : 'no data';
}
?>
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta http-equiv="refresh" content="5">
<title>Smart Campus Dashboard</title>
<style>
  body { background: #000; color: #fff; font-family: Arial, sans-serif; max-width: 760px; margin: 0 auto; padding: 24px; }
  h1 { font-size: 26px; }
  h2 { font-size: 13px; color: #888; text-transform: uppercase; letter-spacing: 1px; margin: 28px 0 10px; }
  .muted { color: #888; }
  .offline { color: #f66; }
  .grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(200px, 1fr)); gap: 12px; }
  .box { border: 1px solid #444; padding: 16px; }
  .box small { color: #888; display: block; }
  .box b { font-size: 28px; font-weight: normal; display: block; margin-top: 8px; }
  table { width: 100%; border-collapse: collapse; font-size: 14px; }
  th, td { text-align: left; padding: 8px 8px 8px 0; border-bottom: 1px solid #222; }
  th { color: #888; font-weight: normal; }
  .scroll { overflow-x: auto; }
  button { background: #000; color: #fff; border: 1px solid #fff; padding: 6px 14px; cursor: pointer; }
  button:hover { background: #fff; color: #000; }
</style>
</head>
<body>

<h1>Smart Campus Dashboard</h1>
<p class="<?= ($age >= 0 && $age <= 15) ? 'muted' : 'offline' ?>">
  Master: <?= ($age >= 0 && $age <= 15) ? 'ONLINE' : 'OFFLINE' ?>
  (last data: <?= $age >= 0 ? $age . ' seconds ago' : 'never' ?>)
</p>

<h2>Sensor values</h2>
<div class="grid">
  <div class="box"><small>Temperature</small><b><?= v($s, 'temperature', '°C') ?></b></div>
  <div class="box"><small>Humidity</small><b><?= v($s, 'humidity', '%') ?></b></div>
  <div class="box"><small>Water level</small><b><?= v($s, 'water_level', 'cm') ?></b></div>
  <div class="box"><small>Ultrasonic (master)</small><b><?= v($s, 'distance', 'cm') ?></b></div>
  <div class="box"><small>Ultrasonic (slave)</small><b><?= isset($s['slave_distance']) ? v($s, 'slave_distance', 'cm') : 'slave offline' ?></b></div>
</div>

<h2>Saved entries (last 10)</h2>
<div class="scroll">
<table>
  <tr><th>Time</th><th>Temp</th><th>Humidity</th><th>Water level</th><th>Master</th><th>Slave</th></tr>
  <?php if ($recent): foreach ($recent as $r): ?>
  <tr>
    <td><?= date('H:i:s', $r['updated'] ?? 0) ?></td>
    <td><?= v($r, 'temperature', '°C') ?></td>
    <td><?= v($r, 'humidity', '%') ?></td>
    <td><?= v($r, 'water_level', 'cm') ?></td>
    <td><?= v($r, 'distance', 'cm') ?></td>
    <td><?= isset($r['slave_distance']) ? v($r, 'slave_distance', 'cm') : 'offline' ?></td>
  </tr>
  <?php endforeach; else: ?>
  <tr><td colspan="6">No entries yet.</td></tr>
  <?php endif; ?>
</table>
</div>

<h2>Master LED control</h2>
<p>Current command: <?= $ledName ?></p>
<form method="post" style="margin-top:8px">
  <button type="submit" name="command" value="ON">LED ON</button>
  <button type="submit" name="command" value="OFF">LED OFF</button>
</form>

<p class="muted" style="margin-top:30px;font-size:13px">Saving to: <?= htmlspecialchars($where) ?> &middot; updates every 5 seconds</p>
</body>
</html>
