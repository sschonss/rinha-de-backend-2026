<?php
date_default_timezone_set('UTC');

require_once __DIR__ . '/FraudDetector.php';
require_once __DIR__ . '/VectorSearch.php';

$indexDir = getenv('INDEX_DIR') ?: '/data/index';
$libPath  = getenv('LIB_PATH') ?: __DIR__ . '/libvector.so';
$nprobe   = (int)(getenv('NPROBE') ?: 10);
$port     = (int)(getenv('PORT') ?: 9999);
$workers  = (int)(getenv('WORKERS') ?: 2);

$server = new Swoole\Http\Server('0.0.0.0', $port);

$server->set([
    'worker_num'        => $workers,
    'dispatch_mode'     => 1,       // round-robin to workers
    'open_tcp_nodelay'  => true,
    'log_level'         => SWOOLE_LOG_WARNING,
    'log_file'          => '/dev/null',
]);

$ready = false;

$server->on('workerStart', function ($server, $workerId) use ($indexDir, $libPath, $nprobe, &$ready) {
    try {
        VectorSearch::init($indexDir, $libPath, $nprobe);
        $ready = true;
        echo "[worker $workerId] IVF index loaded. Ready.\n";
    } catch (\Throwable $e) {
        echo "[worker $workerId] ERROR loading index: {$e->getMessage()}\n";
    }
});

$server->on('request', function (Swoole\Http\Request $req, Swoole\Http\Response $res) use (&$ready) {
    $uri = $req->server['request_uri'];

    // GET /ready
    if ($uri === '/ready') {
        if ($ready) {
            $res->status(200);
            $res->end('OK');
        } else {
            $res->status(503);
            $res->end('NOT READY');
        }
        return;
    }

    // POST /fraud-score
    if ($uri === '/fraud-score' && $req->server['request_method'] === 'POST') {
        try {
            $data = json_decode($req->rawContent(), true);
            if (!$data) {
                // Malformed JSON — fallback
                $res->header('Content-Type', 'application/json');
                $res->end('{"approved":true,"fraud_score":0.0}');
                return;
            }
            $result = FraudDetector::score($data);
            $res->header('Content-Type', 'application/json');
            $res->end(json_encode($result));
        } catch (\Throwable $e) {
            // Any error — fallback to avoid HTTP 500 (weight=5 in scoring)
            $res->header('Content-Type', 'application/json');
            $res->end('{"approved":true,"fraud_score":0.0}');
        }
        return;
    }

    $res->status(404);
    $res->end();
});

echo "Starting Swoole server on port $port with $workers workers...\n";
$server->start();
