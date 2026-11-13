'use strict';

/**
 * csv.js — streaming CSV parser for Node 22 (CommonJS, no dependencies)
 *
 * Features:
 *   - RFC 4180-style quoted fields
 *   - Escaped quotes ("")
 *   - Embedded newlines inside quoted fields
 *   - CRLF and LF line endings
 *   - Configurable delimiter (default: ',')
 *
 * Exports:
 *   CsvParser  — Transform stream, emits one string[] per row
 *   parseAll   — convenience: string → string[][]
 */

const { Transform } = require('node:stream');
const { Readable } = require('node:stream');

// ---------------------------------------------------------------------------
// Parser state machine
// ---------------------------------------------------------------------------

const S_FIELD_START  = 0; // beginning of a new field
const S_UNQUOTED     = 1; // inside an unquoted field
const S_QUOTED       = 2; // inside a quoted field
const S_QUOTE_SEEN   = 3; // just saw a closing/escape quote inside quoted field

/**
 * Low-level incremental CSV parser.
 * Feed chunks with push(chunk), collect rows via onRow(fields[]).
 *
 * @param {object}   opts
 * @param {string}   [opts.delimiter=',']  Single-character field delimiter.
 * @param {function} opts.onRow            Called with string[] for each complete row.
 */
class IncrementalCsvParser {
  constructor({ delimiter = ',', onRow }) {
    if (delimiter.length !== 1) {
      throw new RangeError('delimiter must be exactly one character');
    }
    this._delim  = delimiter;
    this._onRow  = onRow;
    this._state  = S_FIELD_START;
    this._field  = '';   // accumulator for current field
    this._fields = [];   // accumulator for current row
    this._prevCR = false; // tracks a pending \r
  }

  /**
   * Push a string chunk into the parser.
   * @param {string} chunk
   */
  push(chunk) {
    const delim = this._delim;

    for (let i = 0; i < chunk.length; i++) {
      const ch = chunk[i];

      // Absorb the LF half of a CRLF we already handled via \r
      if (this._prevCR && ch === '\n') {
        this._prevCR = false;
        continue;
      }
      this._prevCR = false;

      switch (this._state) {
        case S_FIELD_START:
          if (ch === '"') {
            this._state = S_QUOTED;
          } else if (ch === delim) {
            // empty unquoted field
            this._fields.push('');
          } else if (ch === '\r' || ch === '\n') {
            if (ch === '\r') this._prevCR = true;
            this._endRow();
          } else {
            this._field += ch;
            this._state  = S_UNQUOTED;
          }
          break;

        case S_UNQUOTED:
          if (ch === delim) {
            this._endField();
          } else if (ch === '\r' || ch === '\n') {
            if (ch === '\r') this._prevCR = true;
            this._endField();
            this._endRow();
          } else {
            this._field += ch;
          }
          break;

        case S_QUOTED:
          if (ch === '"') {
            this._state = S_QUOTE_SEEN;
          } else {
            this._field += ch;
          }
          break;

        case S_QUOTE_SEEN:
          if (ch === '"') {
            // escaped quote inside quoted field
            this._field += '"';
            this._state  = S_QUOTED;
          } else if (ch === delim) {
            this._endField();
          } else if (ch === '\r' || ch === '\n') {
            if (ch === '\r') this._prevCR = true;
            this._endField();
            this._endRow();
          } else {
            // Lenient: treat as end of quoted field, continue as unquoted
            this._field += ch;
            this._state  = S_UNQUOTED;
          }
          break;
      }
    }
  }

  /**
   * Signal end of input. Flushes any in-progress row.
   */
  flush() {
    // If there is any pending content, emit the final row.
    if (this._fields.length > 0 || this._field !== '' || this._state !== S_FIELD_START) {
      // End the current field regardless of quote state
      this._endField();
      this._endRow();
    }
  }

  _endField() {
    this._fields.push(this._field);
    this._field = '';
    this._state = S_FIELD_START;
  }

  _endRow() {
    // Skip completely empty rows (e.g. trailing newline)
    if (this._fields.length > 0) {
      this._onRow(this._fields);
    }
    this._fields = [];
    this._state  = S_FIELD_START;
  }
}

// ---------------------------------------------------------------------------
// Transform stream
// ---------------------------------------------------------------------------

/**
 * A Transform stream that accepts Buffer/string chunks and emits one string[]
 * per CSV row as an object-mode output.
 *
 * @example
 * const parser = new CsvParser({ delimiter: ';' });
 * parser.on('data', row => console.log(row));
 * fs.createReadStream('data.csv').pipe(parser);
 */
class CsvParser extends Transform {
  /**
   * @param {object} [opts]
   * @param {string} [opts.delimiter=',']
   * @param {BufferEncoding} [opts.encoding='utf8']  Input encoding when chunks are Buffers.
   */
  constructor(opts = {}) {
    const { delimiter = ',', encoding = 'utf8', ...rest } = opts;
    super({ ...rest, readableObjectMode: true });
    this._encoding = encoding;
    this._inner = new IncrementalCsvParser({
      delimiter,
      onRow: (fields) => this.push(fields),
    });
  }

  _transform(chunk, encoding, callback) {
    try {
      const str = Buffer.isBuffer(chunk) ? chunk.toString(this._encoding) : chunk;
      this._inner.push(str);
      callback();
    } catch (err) {
      callback(err);
    }
  }

  _flush(callback) {
    try {
      this._inner.flush();
      callback();
    } catch (err) {
      callback(err);
    }
  }
}

// ---------------------------------------------------------------------------
// Convenience helper
// ---------------------------------------------------------------------------

/**
 * Parse an entire CSV string synchronously (via stream internals).
 *
 * @param {string}  input
 * @param {object}  [opts]
 * @param {string}  [opts.delimiter=',']
 * @returns {string[][]}
 */
function parseAll(input, opts = {}) {
  const { delimiter = ',' } = opts;
  const rows = [];
  const parser = new IncrementalCsvParser({
    delimiter,
    onRow: (fields) => rows.push(fields),
  });
  parser.push(input);
  parser.flush();
  return rows;
}

// ---------------------------------------------------------------------------
// Demo
// ---------------------------------------------------------------------------

function runDemo() {
  const sample = [
    'id,name,note',
    '1,Alice,"loves ""quoting""  things"',
    '2,Bob,"line one\r\nline two"',
    '3,"Charlie, Jr.",no quotes here',
    '4,Dana,',
  ].join('\r\n');

  console.log('=== parseAll demo ===');
  const rows = parseAll(sample);
  for (const row of rows) {
    console.log(row);
  }

  console.log('\n=== Transform stream demo ===');
  const readable = Readable.from([sample]);
  const parser   = new CsvParser();
  readable.pipe(parser);
  parser.on('data', (row) => console.log(row));
  parser.on('end',  () => console.log('stream done'));
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

module.exports = { CsvParser, parseAll };

// Run demo when executed directly: `node csv.js`
if (require.main === module) {
  runDemo();
}
