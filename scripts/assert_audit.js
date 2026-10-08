'use strict';
// assert_audit.js -- proves a refactor did not weaken or delete any test assertion.
//
//   node scripts/assert_audit.js [revision]        (default: HEAD)
//
// For every test file the working tree changes, it takes the assertions
// (EXPECT_*/ASSERT_*) from the given revision and from the working tree,
// normalises away the mechanical rewrites this port has used, and compares the
// multisets.  The only differences it tolerates are:
//   * the expression wrapper around the same assertion changing
//     (`.asJson()` while the value type was erased, `jsonView(...)` after the
//     JSON layer moved to its own header), and
//   * added assertions.
// Anything that "disappeared" is printed, so a deleted or weakened assertion
// fails the run instead of hiding in a large diff.
//
// Normalisation happens over the WHOLE text (chains may span lines):
//   1. `x.asJson().y` / `x->asJson().y` -> `x.y` / `x->y`
//   2. `jsonView(EXPR).y`              -> `EXPR.y`
//      `jsonView(*EXPR).y`             -> `EXPR->y`   (was `EXPR->asJson().y`)
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const repo = path.join(__dirname, '..');
const revision = process.argv[2] || 'HEAD';

const files = execFileSync('git', ['diff', '--name-only', '--', 'test'], { cwd: repo })
  .toString().trim().split(/\r?\n/).filter(Boolean);

// Turns `jsonView(<expr>)` back into `<expr>` (balanced parens, may span lines).
function unwrapJsonView(text) {
  const marker = 'jsonView(';
  let out = text;
  for (;;) {
    const at = out.indexOf(marker);
    if (at < 0) return out;
    let depth = 0;
    let end = -1;
    for (let i = at + marker.length - 1; i < out.length; i++) {
      const c = out[i];
      if (c === '(') depth++;
      else if (c === ')') {
        depth--;
        if (depth === 0) { end = i; break; }
      }
    }
    if (end < 0) return out;  // unbalanced: leave the text alone
    const inner = out.slice(at + marker.length, end);
    const deref = inner.startsWith('*');
    const expr = deref ? inner.slice(1) : inner;
    // `->asJson()` used to be "deref then take the node", so it becomes `expr->…`.
    const tail = out.slice(end + 1);
    const joined = deref && tail.startsWith('.') ? expr + '->' + tail.slice(1) : expr + tail;
    out = out.slice(0, at) + joined;
  }
}

const normalize = (text) =>
  unwrapJsonView(text)
      // The erased-value era inserted `.asJson()` purely to unwrap a handle.
      .replace(/(->|\.)asJson\(\)\./g, '$1');

const isAssertion = (line) => /^\s*(EXPECT|ASSERT)_[A-Z_]+\(/.test(line);
const collapse = (line) => line.trim().replace(/\s+/g, ' ');

function assertions(text) {
  const counts = new Map();
  for (const line of normalize(text).split(/\r?\n/)) {
    if (!isAssertion(line)) continue;
    const key = collapse(line);
    counts.set(key, (counts.get(key) || 0) + 1);
  }
  return counts;
}

let problems = 0;
for (const file of files) {
  let before;
  try {
    before = assertions(execFileSync('git', ['show', revision + ':' + file], { cwd: repo }).toString());
  } catch (e) {
    console.log('NEW FILE (no ' + revision + ' version): ' + file);
    continue;
  }
  const after = assertions(fs.readFileSync(path.join(repo, file), 'utf8'));

  const missing = [];
  for (const [line, n] of before) {
    const now = after.get(line) || 0;
    if (now < n) missing.push(line + '  (x' + n + ' -> x' + now + ')');
  }
  const added = [];
  for (const [line, n] of after) {
    const was = before.get(line) || 0;
    if (n > was) added.push(line + '  (x' + was + ' -> x' + n + ')');
  }

  const totalBefore = [...before.values()].reduce((a, b) => a + b, 0);
  const totalAfter = [...after.values()].reduce((a, b) => a + b, 0);
  const flag = missing.length ? 'MISSING' : 'ok';
  console.log(`${flag.padEnd(8)} ${file.padEnd(44)} assertions ${totalBefore} -> ${totalAfter}` +
              (added.length ? '  (+' + added.length + ' new)' : ''));
  if (missing.length) {
    problems += missing.length;
    for (const line of missing) console.log('    missing: ' + line);
  }
}
console.log(problems === 0 ? '\nNO ASSERTION WAS WEAKENED OR REMOVED'
                           : '\n' + problems + ' ASSERTION(S) MISSING');
process.exit(problems === 0 ? 0 : 1);
