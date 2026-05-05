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

// Pre-computed JSON responses indexed by fraud_count (0-5)
const RESPONSES = [
    '{"approved":true,"fraud_score":0}',
    '{"approved":true,"fraud_score":0.2}',
    '{"approved":true,"fraud_score":0.4}',
    '{"approved":false,"fraud_score":0.6}',
    '{"approved":false,"fraud_score":0.8}',
    '{"approved":false,"fraud_score":1}',
];

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

    // POST /fraud-score — entire pipeline in C (yyjson parse + vectorize + search)
    if ($uri === '/fraud-score' && $req->server['request_method'] === 'POST') {
        try {
            $body = $req->rawContent();
            if (!$body) {
                $res->header('Content-Type', 'application/json');
                $res->end(RESPONSES[0]);
                return;
            }
            $fraudCount = VectorSearch::processRequest($body);
            if ($fraudCount < 0) $fraudCount = 0;
            $res->header('Content-Type', 'application/json');
            $res->end(RESPONSES[$fraudCount]);
        } catch (\Throwable $e) {
            $res->header('Content-Type', 'application/json');
            $res->end(RESPONSES[0]);
        }
        return;
    }

    $res->status(404);
    $res->end();
});

echo "Starting Swoole server on port $port with $workers workers...\n";
$server->start();
