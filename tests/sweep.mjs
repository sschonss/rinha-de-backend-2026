// Sweeps every transaction in test-data.json once, prints any disagreement
// between expected_approved and our /fraud-score response.
import fs from 'fs';

const URL = 'http://localhost:9999/fraud-score';
const data = JSON.parse(fs.readFileSync('/tmp/rinha-spec/test/test-data.json', 'utf8'));
console.log(`Loaded ${data.entries.length} entries`);

let fp = 0, fn = 0, errs = 0;
const wrong = [];

const CONCURRENCY = 32;
let next = 0;

async function worker(workerId) {
    while (true) {
        const i = next++;
        if (i >= data.entries.length) return;
        const e = data.entries[i];
        try {
            const r = await fetch(URL, {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(e.request),
            });
            if (r.status !== 200) { errs++; continue; }
            const body = await r.json();
            if (body.approved !== e.expected_approved) {
                if (body.approved) {
                    fn++;  // fraud approved
                    wrong.push({ idx: i, kind: 'FN', expected: e.expected_approved, got: body, req: e.request });
                } else {
                    fp++;  // legit denied
                    wrong.push({ idx: i, kind: 'FP', expected: e.expected_approved, got: body, req: e.request });
                }
            }
        } catch (err) {
            errs++;
        }
        if (i % 5000 === 0) console.log(`worker${workerId} idx=${i} fn=${fn} fp=${fp} errs=${errs}`);
    }
}

const t0 = Date.now();
await Promise.all(Array.from({ length: CONCURRENCY }, (_, i) => worker(i)));
const dt = (Date.now() - t0) / 1000;
console.log(`\nDone in ${dt.toFixed(1)}s. FN=${fn} FP=${fp} errs=${errs}`);
console.log(`\nWrong transactions (${wrong.length}):`);
for (const w of wrong) {
    console.log(`\n[${w.kind}] idx=${w.idx} expected_approved=${w.expected} got=${JSON.stringify(w.got)}`);
    console.log(`  request: ${JSON.stringify(w.req)}`);
}
