'use strict';

const { Transform } = require('stream');

/*
 * csv.js — Streaming CSV parser for Node.js
 * No dependencies. Node 22 CommonJS.
 *
 * Features:
 *   - Quoted fields with escaped quotes ("")
 *   - Embedded newlines inside quoted fields
 *   - CRLF, LF, and lone CR line endings (CRLF normalized)
 *   - Configurable single-char delimiter (default ",")
 *   - Exposed as a Transform stream (object-mode output, one array per row)
 *   - parseAll convenience (async + sync)
 *
 * Usage:
 *   const { CsvParser, parseAll } = require('./csv');
 *
 *   // Streaming — pipe a readable or write directly
 *   const parser = new CsvParser({ delimiter: '\t' });
 *   parser.on('data', row => console.log(row));
 *   someReadable.pipe(parser);
 *
 *   // Async convenience
 *   const rows = await parseAll('a,b\n1,2');
 *
 *   // Sync convenience
 *   const rows = parseAll.sync('a,b\n1,2');
 */

// ---------------------------------------------------------------------------
// State-machine constants
// ---------------------------------------------------------------------------

const S_FIELD_START     = 0; // expecting first char of a new field
const S_UNQUOTED        = 1; // inside an unquoted field
const S_QUOTED          = 2; // inside a quoted field
const S_QUOTE_IN_QUOTED = 3; // just saw a quote inside a quoted field
const S_AFTER_CR        = 4; // just saw \r outside quotes (CRLF or lone CR)

// ---------------------------------------------------------------------------
// Core parser — shared by the stream and the sync API
// ---------------------------------------------------------------------------

/**
 * Feed a string chunk through the state machine.
 *
 * @param {string}   str              input text to process
 * @param {object}   ctx              mutable state object
 * @param {string}   delimiter        single-char field separator
 * @param {string}   quote            single-char quote character
 * @param {boolean}  skipEmptyLines   suppress rows that are a lone empty field
 * @param {function(string[]):void} onRow  called with each completed row
 */
function processChunk(str, ctx, delimiter, quote, skipEmptyLines, onRow) {
  let i = 0;
  while (i < str.length) {
    const ch = str[i];

    switch (ctx.state) {
      case S_FIELD_START:
        ctx.hasContent = false;
        if (ch === quote) {
          // A quoted field is real content even if it turns out empty.
          ctx.hasContent = true;
          ctx.state = S_QUOTED;
          i++;
        } else if (ch === delimiter) {
          ctx.row.push(ctx.field);
          ctx.field = '';
          i++;
        } else if (ch === '\n') {
          ctx.row.push(ctx.field);
          flushRow(ctx, skipEmptyLines, onRow);
          i++;
        } else if (ch === '\r') {
          ctx.row.push(ctx.field);
          ctx.state = S_AFTER_CR;
          i++;
        } else {
          ctx.field += ch;
          ctx.hasContent = true;
          ctx.state = S_UNQUOTED;
          i++;
        }
        break;

      case S_UNQUOTED:
        if (ch === delimiter) {
          ctx.row.push(ctx.field);
          ctx.field = '';
          ctx.state = S_FIELD_START;
          i++;
        } else if (ch === '\n') {
          ctx.row.push(ctx.field);
          flushRow(ctx, skipEmptyLines, onRow);
          i++;
        } else if (ch === '\r') {
          ctx.row.push(ctx.field);
          ctx.state = S_AFTER_CR;
          i++;
        } else {
          ctx.field += ch;
          i++;
        }
        break;

      case S_QUOTED:
        if (ch === quote) {
          ctx.state = S_QUOTE_IN_QUOTED;
          i++;
        } else {
          // Inside quotes: delimiter, CR, LF are all literal field content.
          ctx.field += ch;
          i++;
        }
        break;

      case S_QUOTE_IN_QUOTED:
        if (ch === quote) {
          // Doubled quote → one literal quote character.
          ctx.field += quote;
          ctx.state = S_QUOTED;
          i++;
        } else if (ch === delimiter) {
          ctx.row.push(ctx.field);
          ctx.field = '';
          ctx.state = S_FIELD_START;
          i++;
        } else if (ch === '\n') {
          ctx.row.push(ctx.field);
          flushRow(ctx, skipEmptyLines, onRow);
          i++;
        } else if (ch === '\r') {
          ctx.row.push(ctx.field);
          ctx.state = S_AFTER_CR;
          i++;
        } else {
          // Non-standard: text after the closing quote in the same field.
          // Treat as unquoted continuation so data is not silently dropped.
          ctx.field += ch;
          ctx.state = S_UNQUOTED;
          i++;
        }
        break;

      case S_AFTER_CR:
        if (ch === '\n') {
          // CRLF — the \r already ended the field; \n confirms end of row.
          flushRow(ctx, skipEmptyLines, onRow);
          i++;
        } else {
          // Lone CR — it was a line ending on its own.
          flushRow(ctx, skipEmptyLines, onRow);
          // Do not increment i: reprocess the current character from S_FIELD_START.
        }
        break;
    }
  }
}

/**
 * Emit a completed row, honouring the skipEmptyLines option.
 */
function flushRow(ctx, skipEmptyLines, onRow) {
  if (!(skipEmptyLines && ctx.row.length === 1 && ctx.row[0] === '' && !ctx.hasContent)) {
    onRow(ctx.row);
  }
  ctx.row = [];
  ctx.field = '';
  ctx.hasContent = false;
}

/**
 * Handle remaining state when the input ends (stream _flush / sync tail).
 */
function finalize(ctx, skipEmptyLines, onRow) {
  switch (ctx.state) {
    case S_QUOTED:
    case S_QUOTE_IN_QUOTED:
    case S_UNQUOTED:
      // Mid-field or unclosed quote — emit what we have so far.
      ctx.row.push(ctx.field);
      flushRow(ctx, skipEmptyLines, onRow);
      break;
    case S_FIELD_START:
      // A trailing delimiter (e.g. "a,") leaves a pending empty field.
      if (ctx.row.length > 0 || ctx.field !== '') {
        ctx.row.push(ctx.field);
        flushRow(ctx, skipEmptyLines, onRow);
      }
      break;
    case S_AFTER_CR:
      // Stream ended right after \r — treat it as a completed row.
      flushRow(ctx, skipEmptyLines, onRow);
      break;
    // S_FIELD_START with empty row and field: nothing to emit.
  }
}

// ---------------------------------------------------------------------------
// Transform stream
// ---------------------------------------------------------------------------

class CsvParser extends Transform {
  /**
   * @param {object}  [options]
   * @param {string}  [options.delimiter=',']   single-char field separator
   * @param {string}  [options.quote='"']        single-char quote character
   * @param {string}  [options.encoding='utf8']  text encoding for Buffer chunks
   * @param {boolean} [options.skipEmptyLines=true]  skip rows that are a lone empty field
   */
  constructor(options = {}) {
    super({
      ...options,
      readableObjectMode: true,
      decodeStrings: false, // keep strings as strings in _transform
    });
    this._delimiter = options.delimiter != null ? options.delimiter : ',';
    this._quote = options.quote != null ? options.quote : '"';
    this._encoding = options.encoding || 'utf8';
    this._skipEmptyLines = options.skipEmptyLines !== false;
    this._ctx = {
      state: S_FIELD_START,
      row: [],
      field: '',
      hasContent: false,
    };
  }

  _transform(chunk, encoding, callback) {
    const str = Buffer.isBuffer(chunk)
      ? chunk.toString(this._encoding)
      : typeof chunk === 'string' ? chunk : String(chunk);

    processChunk(
      str,
      this._ctx,
      this._delimiter,
      this._quote,
      this._skipEmptyLines,
      (row) => this.push(row)
    );
    callback();
  }

  _flush(callback) {
    finalize(
      this._ctx,
      this._skipEmptyLines,
      (row) => this.push(row)
    );
    callback();
  }
}

// ---------------------------------------------------------------------------
// Convenience API
// ---------------------------------------------------------------------------

/**
 * Parse a string or Buffer and resolve with all rows.
 *
 * @param {string|Buffer} input
 * @param {object}        [options]  same options as CsvParser constructor
 * @returns {Promise<string[][]>}
 */
async function parseAll(input, options = {}) {
  const parser = new CsvParser(options);
  const rows = [];

  return new Promise((resolve, reject) => {
    parser.on('data', (row) => rows.push(row));
    parser.on('end', () => resolve(rows));
    parser.on('error', reject);
    parser.write(input);
    parser.end();
  });
}

/**
 * Parse a string or Buffer synchronously and return all rows.
 *
 * @param {string|Buffer} input
 * @param {object}        [options]  same options as CsvParser constructor
 * @returns {string[][]}
 */
parseAll.sync = function parseAllSync(input, options = {}) {
  const delimiter = options.delimiter != null ? options.delimiter : ',';
  const quote = options.quote != null ? options.quote : '"';
  const encoding = options.encoding || 'utf8';
  const skipEmptyLines = options.skipEmptyLines !== false;

  const str = Buffer.isBuffer(input) ? input.toString(encoding) : String(input);
  const ctx = { state: S_FIELD_START, row: [], field: '', hasContent: false };
  const rows = [];

  processChunk(str, ctx, delimiter, quote, skipEmptyLines, (row) => rows.push(row));
  finalize(ctx, skipEmptyLines, (row) => rows.push(row));
  return rows;
};

// ---------------------------------------------------------------------------
// Demo — runs with `node csv.js`
// ---------------------------------------------------------------------------

if (require.main === module) {
  // A sample string exercising every parser feature: quoted fields with
  // embedded delimiters, escaped quotes, embedded newlines, and empty fields.
  const sample = [
    'name,age,note',
    'Alice,30,"Hello, world"',
    'Bob,25,"She said ""hi"" and left"',
    'Carol,35,"First line' + '\n' + 'Second line"',
    'Dave,40,plain',
    'Eve,,',
  ].join('\n');

  console.log('=== Input ===');
  console.log(sample);
  console.log();

  console.log('=== parseAll.sync ===');
  for (const row of parseAll.sync(sample)) {
    console.log(' ', row);
  }

  console.log('\n=== Streaming via CsvParser ===');
  const parser = new CsvParser();
  parser.on('data', (row) => console.log(' ', row));
  parser.on('end', () => {
    console.log('\n=== Custom delimiter (tab) ===');
    const tsv = 'id\tlabel\tactive\n1\t"hello\tworld"\tyes\n2\ttab\tno';
    for (const row of parseAll.sync(tsv, { delimiter: '\t' })) {
      console.log(' ', row);
    }
  });
  parser.end(sample);
}

module.exports = { CsvParser, parseAll };
