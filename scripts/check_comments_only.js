'use strict';
// check_comments_only.js <original> <changed>
//
// Verifies that a change to a source file touched comments only -- this is how the
// Chinese translation of include/codec.hpp's comments was checked: the header's code
// is byte-for-byte the code it had before, every comment line was replaced in place.
//
//   * code identical -- both files are stripped of comments (string/char/raw-string
//     literals kept verbatim), whitespace is collapsed, results compared.  Any
//     change outside a comment -- identifier, literal, number, char literal,
//     preprocessor line, statement order, even code indentation -- shows up here.
//   * line count identical -- one output line per input line, so translated chunks
//     can be concatenated back into the original file structure.
//
// exit 0 = both checks pass, 1 = mismatch (diagnosis printed), 2 = usage/IO.
//
//   node scripts/check_comments_only.js include/codec.hpp translated.hpp
//
// Self-test (checks the checker): node scripts/check_comments_only_selftest.js

const fs = require('fs');

function stripComments(src) {
  let out = '';
  let i = 0;
  const n = src.length;

  const looksLikeRawPrefix = () => {
    // walk back over the already emitted text: R, u8R, uR, UR, LR
    let j = out.length - 1;
    if (j < 0 || out[j] !== 'R') return false;
    j--;
    if (j < 0) return true;
    if (out[j] === 'u' || out[j] === 'U' || out[j] === 'L') return true;
    if (j >= 1 && out[j] === '8' && out[j - 1] === 'u') return true;
    return !/[A-Za-z0-9_$]/.test(out[j]);
  };

  while (i < n) {
    const c = src[i];

    if (c === '/' && src[i + 1] === '/') {
      i += 2;
      while (i < n && src[i] !== '\n') {
        if (src[i] === '\\' && src[i + 1] === '\r' && src[i + 2] === '\n') { i += 3; continue; }
        if (src[i] === '\\' && src[i + 1] === '\n') { i += 2; continue; }
        i++;
      }
      continue; // the newline itself is copied by the loop below
    }

    if (c === '/' && src[i + 1] === '*') {
      i += 2;
      while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i++;
      i += 2;
      out += ' ';
      continue;
    }

    if (c === '"') {
      if (looksLikeRawPrefix()) {
        i++; // the opening quote of R"delim(
        let delim = '';
        while (i < n && src[i] !== '(') { delim += src[i]; i++; }
        i++; // '('
        const closer = ')' + delim + '"';
        const at = src.indexOf(closer, i);
        const end = at === -1 ? n : at + closer.length;
        out += src.slice(i, end);
        i = end;
        continue;
      }
      out += c;
      i++;
      while (i < n) {
        if (src[i] === '\\') { out += src[i] + (src[i + 1] || ''); i += 2; continue; }
        out += src[i];
        if (src[i] === '"') { i++; break; }
        i++;
      }
      continue;
    }

    if (c === "'") {
      out += c;
      i++;
      while (i < n) {
        if (src[i] === '\\') { out += src[i] + (src[i + 1] || ''); i += 2; continue; }
        out += src[i];
        if (src[i] === "'") { i++; break; }
        i++;
      }
      continue;
    }

    out += c;
    i++;
  }
  return out;
}

const normalize = (s) => s.replace(/\s+/g, ' ').trim();

function compare(left, right) {
  const a = normalize(stripComments(left));
  const b = normalize(stripComments(right));
  if (a === b) return { same: true, chars: a.length };
  let k = 0;
  while (k < a.length && k < b.length && a[k] === b[k]) k++;
  return { same: false, at: k, left: a, right: b };
}

function main() {
  const [a, b] = process.argv.slice(2);
  if (!a || !b) {
    console.error('usage: node check.js <original> <translated>');
    process.exit(2);
  }
  const srcA = fs.readFileSync(a, 'utf8');
  const srcB = fs.readFileSync(b, 'utf8');
  const result = compare(srcA, srcB);
  const linesA = srcA.split('\n').length;
  const linesB = srcB.split('\n').length;

  if (!result.same) {
    const k = result.at;
    const ctx = (s) => JSON.stringify(s.slice(Math.max(0, k - 60), k + 60));
    console.error('CODE DIFFERS at non-comment char ' + k + ' of ' + result.left.length +
                  '/' + result.right.length);
    console.error('  original   : ' + ctx(result.left));
    console.error('  translated : ' + ctx(result.right));
    console.error('Every character outside a comment must be preserved exactly.');
    process.exit(1);
  }

  console.log('CODE IDENTICAL (' + result.chars + ' chars of non-comment text)');
  if (linesA !== linesB) {
    console.error('LINES DIFFER: ' + linesA + ' lines in the original, ' + linesB +
                  ' in the translation. Keep exactly one output line per input line.');
    process.exit(1);
  }
  console.log('LINES OK (' + linesA + ')');
  process.exit(0);
}

module.exports = { stripComments, normalize, compare };

if (require.main === module) {
  main();
}
