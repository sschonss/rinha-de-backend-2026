<?php

class VectorSearch
{
    private static ?\FFI $ffi = null;
    private static ?\FFI\CData $queryBuf = null;
    private static ?\FFI\CData $labelsBuf = null;
    private static ?\FFI\CData $distsBuf = null;
    private static bool $initialized = false;

    public static function init(string $indexDir, string $libPath = null, int $nprobe = 10): void
    {
        if (self::$initialized) return;

        $libPath = $libPath ?? __DIR__ . '/libvector.so';

        self::$ffi = \FFI::cdef("
            int ivf_init(const char *index_dir, int nprobe);
            int ivf_search(const float *query, int *out_labels, float *out_distances, int k);
            void ivf_destroy(void);
        ", $libPath);

        $result = self::$ffi->ivf_init($indexDir, $nprobe);
        if ($result !== 0) {
            throw new \RuntimeException("Failed to initialize IVF index from $indexDir");
        }

        // Pre-allocate buffers to avoid per-request allocation
        self::$queryBuf = self::$ffi->new("float[14]");
        self::$labelsBuf = self::$ffi->new("int[5]");
        self::$distsBuf = self::$ffi->new("float[5]");
        self::$initialized = true;
    }

    /**
     * @param float[] $vector Array of 14 floats
     * @return int[] Array of 5 labels (0=legit, 1=fraud)
     */
    public static function query(array $vector): array
    {
        if (!self::$initialized) {
            throw new \RuntimeException("VectorSearch not initialized. Call init() first.");
        }

        for ($i = 0; $i < 14; $i++) {
            self::$queryBuf[$i] = $vector[$i];
        }

        self::$ffi->ivf_search(self::$queryBuf, self::$labelsBuf, self::$distsBuf, 5);

        $labels = [];
        for ($i = 0; $i < 5; $i++) {
            $labels[] = self::$labelsBuf[$i];
        }
        return $labels;
    }

    public static function destroy(): void
    {
        if (self::$ffi && self::$initialized) {
            self::$ffi->ivf_destroy();
            self::$initialized = false;
        }
    }
}
