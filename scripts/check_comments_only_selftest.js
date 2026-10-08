'use strict';
// check_comments_only_selftest.js -- proves check_comments_only.js distinguishes
// "comment changed" from "code changed", so a green check really means the code is
// untouched.
//
//   node scripts/check_comments_only_selftest.js [path/to/codec.hpp]
//
// The comment mutations are derived from a comment that actually exists in the file,
// and every mutation asserts that it matched something, so a drifted pattern fails
// loudly instead of silently passing.

const fs = require('fs');
const path = require('path');
const { compare } = require('./check_comments_only.js');

const source = fs.readFileSync(
    process.argv[2] || path.join(__dirname, '..', 'include', 'codec.hpp'), 'utf8');
// Work on LF so multi-line patterns are readable; compare() collapses whitespace.
const original = source.replace(/\r\n/g, '\n');

const sub = (pattern, replacement) => (s) => {
  if (typeof pattern === 'string' && !s.includes(pattern)) return null;
  if (pattern instanceof RegExp && !pattern.test(s)) return null;
  return s.replace(pattern, replacement);
};

// A comment line that exists in the file: used for the comment-only mutations, so
// they never go stale when the comments are (re)translated.
const commentLine = original.split('\n').find((l) => /^\s*\/\/ .*[^\x00-\x7F]/.test(l));
if (!commentLine) throw new Error('no non-ASCII comment line found in ' + path.resolve(source));
const swapComment = (replacement) =>
    (s) => (s.includes(commentLine) ? s.replace(commentLine, '// ' + replacement) : null);

// [name, mutation, expectSameCode]
const cases = [
  ['identity', (s) => s, true],

  // --- comment-only changes must count as "same code" ---------------------
  ['replace a whole-line comment', swapComment('替换后的中文注释，用于自检。'), true],
  ['comment text containing // and quotes', swapComment('文本 "risks[0].op" 与 // 注释'), true],
  ['comment text containing /* */ and an apostrophe', swapComment("文本 /* 说明 */ 与 DFU's 消息"), true],
  ['comment text containing a raw-string lookalike', swapComment('R"(not a string)"'), true],
  ['comment text containing backslashes', swapComment('路径写作 risks\\[0\\]'), true],
  ['comment text containing quotes and parens', swapComment('它说"你好"（自检）'), true],
  ['comment text containing a lone quote and a colon', swapComment("it's 1: 说明"), true],
  ['add a comment line', (s) => '// 新增说明\n' + s, true],
  ['delete a comment line', (s) => s.replace(commentLine + '\n', ''), true],
  ['reflow whitespace inside a comment',
    (s) => s.replace(commentLine, commentLine.replace('// ', '//     ')), true],

  // --- real code changes must be detected ---------------------------------
  ['rename an identifier',
    sub('struct ErrorPart {', 'struct ErrorPartX {'), false],
  ['rename a member',
    sub('std::vector<std::string> frames;', 'std::vector<std::string> framesX;'), false],
  ['change an error-message literal in code',
    sub('"Not a number: "', '"not a number: "'), false],
  ['change a number in code',
    sub('int32_t position = 0;', 'int32_t position = 1;'), false],
  ['comment out a code line',
    sub(/^(\s*)return renderPath\(errors_\.front\(\)\.path\);$/m,
        '$1// return renderPath(errors_.front().path);'), false],
  ['delete a code line',
    sub(/^\s*return renderPath\(errors_\.front\(\)\.path\);\n/m, ''), false],
  ['change a char literal',
    sub('isIndex ? "[" + std::to_string(position) + "]"',
        'isIndex ? "{" + std::to_string(position) + "}"'), false],
  ['change a preprocessor line',
    sub(/^inline const Codec<int32_t> Int =/m, 'static const Codec<int32_t> Int ='), false],
  ['add a preprocessor line',
    (s) => '#define CODEC_EXTRA 1\n' + s, false],
  ['change code indentation only',
    sub('const std::string& frame', 'const std::string & frame'), false],
  ['reorder two code statements',
    sub(/out \+= "\\n  in ";\n(\s*)out \+= frame;/,
        'out += frame;\n$1out += "\\n  in ";'), false],
  ['add a semicolon after a block',
    sub(/^(\s*)return \*this;\n(\s*)\}$/m, '$1return *this;\n$2};'), false],
  ['swap two function arguments',
    sub('ops.mergeToPrimitive(prefix, write(ops, input))',
        'ops.mergeToPrimitive(write(ops, input), prefix)'), false],
  ['change a std::move into a copy',
    sub('std::vector<JsonValue> out;', 'std::vector<JsonValue> out = {};'), false],
];

let failures = 0;
for (const [name, mutate, expectSame] of cases) {
  let mutated;
  try {
    mutated = mutate(original);
  } catch (e) {
    console.log('ERROR  ' + name + ': mutation threw ' + e.message);
    failures++;
    continue;
  }
  if (mutated === null || (mutated === original && expectSame === false)) {
    console.log('STALE  ' + name + ': the mutation matched nothing (pattern drifted)');
    failures++;
    continue;
  }
  const result = compare(original, mutated);
  const ok = result.same === expectSame;
  if (!ok) failures++;
  console.log((ok ? 'ok     ' : 'FAIL   ') + name + '  -> code ' +
              (result.same ? 'identical' : 'differs') + ' (expected ' +
              (expectSame ? 'identical' : 'differs') + ')');
}

console.log(failures === 0 ? '\nALL SELF-TESTS PASS'
                           : '\n' + failures + ' SELF-TEST(S) FAILED');
process.exit(failures === 0 ? 0 : 1);
