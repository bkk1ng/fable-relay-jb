/**
 * csv.js — streaming CSV parser for Node 22 (CommonJS, no dependencies)
 *
 * Features:
 *   - Quoted fields (RFC 4180)
 *   - Escaped quotes ("")
 *   - Embedded newlines inside quoted fields
 *   - CRLF and LF line endings
 *   - Configurable delimiter (default: comma)
 *
 * Exports:
 *   - CsvParser   — Transform stream, emits one string[] per row
 *   - parseAll    — Convenience: parse a string/Buffer, return string[][]
 */

'use strict';

const { Transform } = require('stream');

// ---------------------------------------------------------------------------
// Parser state machine
// ---------------------------------------------------------------------------

const S_FIELD_START  = 0; // between fields / at row start
const S_UNQUOTED     = 1; // inside an unquoted field
const S_QUOTED       = 2; // inside a quoted field
const S_QUOTE_SEEN   = 3; // saw a closing/escape quote inside a quoted field

class CsvParserState {
  constructor(delimiter) {
    this.delimiter   = delimiter;
    this.rows        = [];   // completed rows waiting to be emitted
    this.row         = [];   // current row (array of fields)
    this.field       = '';   // characters accumulated for the current field
    this.state       = S_FIELD_START;
    this.pendingCR   = false; // true when the last char was CR
  }

  /**
   * Feed a chunk of text into the state machine.
   * Returns any completed rows (each row is string[]).
   * @param {string} chunk
   * @returns {string[][]}
   */
  push(chunk) {
    this.rows = [];

    for (let i = 0; i < chunk.length; i++) {
      const ch = chunk[i];

      // Normalise CRLF: skip the LF that follows a CR
      if (this.pendingCR) {
        this.pendingCR = false;
        if (ch === '\n') continue; // the \r already ended the row
      }

      switch (this.state) {
        case S_FIELD_START:
          if (ch === '"') {
            this.state = S_QUOTED;
          } else if (ch === this.delimiter) {
            this.row.push('');         // empty field
          } else if (ch === '\r') {
            this.pendingCR = true;
            this._endRow();
          } else if (ch === '\n') {
            this._endRow();
          } else {
            this.field += ch;
            this.state  = S_UNQUOTED;
          }
          break;

        case S_UNQUOTED:
          if (ch === this.delimiter) {
            this._endField();
            this.state = S_FIELD_START;
          } else if (ch === '\r') {
            this.pendingCR = true;
            this._endField();
            this._endRow();
            this.state = S_FIELD_START;
          } else if (ch === '\n') {
            this._endField();
            this._endRow();
            this.state = S_FIELD_START;
          } else {
            this.field += ch;
          }
          break;

        case S_QUOTED:
          if (ch === '"') {
            this.state = S_QUOTE_SEEN;
          } else {
            this.field += ch;
          }
          break;

        case S_QUOTE_SEEN:
          if (ch === '"') {
            // Escaped quote — literal " in the field value
            this.field += '"';
            this.state  = S_QUOTED;
          } else if (ch === this.delimiter) {
            this._endField();
            this.state = S_FIELD_START;
          } else if (ch === '\r') {
            this.pendingCR = true;
            this._endField();
            this._endRow();
            this.state = S_FIELD_START;
          } else if (ch === '\n') {
            this._endField();
            this._endRow();
            this.state = S_FIELD_START;
          } else {
            // Non-standard: character after closing quote — treat as unquoted
            this.field += ch;
            this.state  = S_UNQUOTED;
          }
          break;
      }
    }

    return this.rows;
  }

  /**
   * Signal end of input. Returns any trailing row not yet emitted.
   * @returns {string[][]}
   */
  flush() {
    this.rows = [];

    // Finish any in-progress field / row
    if (this.field !== '' || this.row.length > 0 || this.state === S_QUOTED) {
      this._endField();
      this._endRow();
    }

    return this.rows;
  }

  _endField() {
    this.row.push(this.field);
    this.field = '';
    this.state = S_FIELD_START;
  }

  _endRow() {
    if (this.row.length > 0) {
      this.rows.push(this.row);
      this.row = [];
    }
  }
}

// ---------------------------------------------------------------------------
// Transform stream
// ---------------------------------------------------------------------------

class CsvParser extends Transform {
  /**
   * @param {object}  [options]
   * @param {string}  [options.delimiter=',']  Single-character field delimiter.
   * @param {object}  [options.transform]      Additional Transform options.
   */
  constructor({ delimiter = ',', ...transformOptions } = {}) {
    if (delimiter.length !== 1) {
      throw new RangeError('delimiter must be exactly one character');
    }

    super({ ...transformOptions, readableObjectMode: true });
    this._csvState = new CsvParserState(delimiter);
  }

  _transform(chunk, encoding, callback) {
    try {
      const text = Buffer.isBuffer(chunk) ? chunk.toString('utf8') : chunk;
      const rows = this._csvState.push(text);
      for (const row of rows) this.push(row);
      callback();
    } catch (err) {
      callback(err);
    }
  }

  _flush(callback) {
    try {
      const rows = this._csvState.flush();
      for (const row of rows) this.push(row);
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
 * Parse an entire CSV string or Buffer synchronously.
 *
 * @param {string|Buffer} input
 * @param {object}        [options]
 * @param {string}        [options.delimiter=',']
 * @returns {string[][]}  Array of rows; each row is an array of field strings.
 */
function parseAll(input, { delimiter = ',' } = {}) {
  const text   = Buffer.isBuffer(input) ? input.toString('utf8') : input;
  const state  = new CsvParserState(delimiter);
  const rows   = state.push(text);
  const tail   = state.flush();
  return rows.concat(tail);
}

// ---------------------------------------------------------------------------
// Demo
// ---------------------------------------------------------------------------

if (require.main === module) {
  const { Readable } = require('stream');

  const csv = [
    'id,name,note',
    '1,Alice,"Loves ""rock climbing""',
    'and hiking"',       // embedded newline in the quoted field above
    '2,Bob,"Berlin, DE"',
    '3,Charlie,',
  ].join('\r\n');

  console.log('--- parseAll demo ---');
  const rows = parseAll(csv);
  rows.forEach((row, i) => console.log(`row ${i}:`, row));

  console.log('\n--- Transform stream demo ---');
  const parser = new CsvParser();

  parser.on('data', (row) => console.log('stream row:', row));
  parser.on('end',  () => console.log('stream done'));

  Readable.from([csv]).pipe(parser);
}

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

module.exports = { CsvParser, parseAll };
