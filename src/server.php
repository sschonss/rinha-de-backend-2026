<?php
date_default_timezone_set('UTC');

require_once __DIR__ . '/FraudDetector.php';
require_once __DIR__ . '/VectorSearch.php';

$indexPath  = getenv('INDEX_PATH') ?: '/data/index.bin';
$libPath    = getenv('LIB_PATH')   ?: __DIR__ . '/libvector.so';
$fastNprobe = (int)(getenv('FAST_NPROBE') ?: 8);
$fullNprobe = (int)(getenv('FULL_NPROBE') ?: 24);
$port       = (int)(getenv('PORT') ?: 9999);
$sockPath   = getenv('SOCK_PATH') ?: '';
$workers    = (int)(getenv('WORKERS') ?: 1);
$warmup     = (int)(getenv('WARMUP') ?: 500);

if ($sockPath) {
    if (file_exists($sockPath)) @unlink($sockPath);
    umask(0);
    $server = new Swoole\Http\Server($sockPath, 0, SWOOLE_BASE, SWOOLE_SOCK_UNIX_STREAM);
} else {
    $server = new Swoole\Http\Server('0.0.0.0', $port);
}

$server->set([
    'worker_num'        => $workers,
    'reactor_num'       => 1,
    'enable_coroutine'  => false,
    'dispatch_mode'     => 1,
    'open_tcp_nodelay'  => true,
    'log_level'         => SWOOLE_LOG_WARNING,
    'log_file'          => '/dev/null',
]);

$ready = false;

try {
    VectorSearch::init($indexPath, $libPath, $fastNprobe, $fullNprobe);
    echo "[master] index loaded (fast=$fastNprobe full=$fullNprobe)\n";
} catch (\Throwable $e) {
    echo "[master] index load FAILED: {$e->getMessage()}\n";
}

$server->on('workerStart', function ($server, $workerId) use ($warmup, &$ready) {
    try {
        if ($warmup > 0) {
            VectorSearch::warmup($warmup);
        }
        $ready = true;
        @file_put_contents('/tmp/ready', '1');
        echo "[worker $workerId] ready (warmup=$warmup)\n";
    } catch (\Throwable $e) {
        $ready = true;
        @file_put_contents('/tmp/ready', '1');
        echo "[worker $workerId] DEGRADED: {$e->getMessage()}\n";
    }
});

$server->on('request', function (Swoole\Http\Request $req, Swoole\Http\Response $res) use (&$ready) {
    $uri = $req->server['request_uri'];

    if ($uri === '/ready') {
        if ($ready) { $res->status(200); $res->end('OK'); }
        else        { $res->status(503); $res->end('NOT READY'); }
        return;
    }

    if ($uri === '/fraud-score' && $req->server['request_method'] === 'POST') {
        try {
            $data = json_decode($req->rawContent(), true);
            if (!$data) {
                $res->header('Content-Type', 'application/json');
                $res->end(FraudDetector::RESPONSES[0]);
                return;
            }
            $json = FraudDetector::scoreToJson($data);
            $res->header('Content-Type', 'application/json');
            $res->end($json);
        } catch (\Throwable $e) {
            $res->header('Content-Type', 'application/json');
            $res->end(FraudDetector::RESPONSES[0]);
        }
        return;
    }

    $res->status(404);
    $res->end();
});

if ($sockPath) {
    echo "Starting Swoole on unix:$sockPath ($workers workers)\n";
} else {
    echo "Starting Swoole on :$port ($workers workers)\n";
}
$server->start();
