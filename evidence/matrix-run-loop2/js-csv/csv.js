'use strict';

// csv.js — streaming CSV parser for Node 22 (CommonJS, zero dependencies)
//
// Handles quoted fields, escaped quotes (doubled "" per RFC 4180), embedded
// newlines inside quotes, CRLF / LF / lone-CR line endings, and a configurable
// field delimiter.  Exposed as a Transform stream plus a parseAll helper.
//
// Run the demo:   node csv.js
// Use as a module: const { CSVParser, parseAll } = require('./csv');

const { Transform } = require('stream');
const { StringDecoder } = require('string_decoder');

/**
 * Streaming CSV parser.
 *
 * @param {object} [options]
 * @param {string} [options.delimiter=',']   Single-character field separator.
 * @param {string} [options.quote='"']       Single-character quote.
 * @param {boolean} [options.skipEmptyLines=true]  Drop lines with no content.
 */
class CSVParser extends Transform {
  constructor(options = {}) {
    super({
      ...options,
      readableObjectMode: true,
      writableObjectMode: false,
      decodeStrings: false, // we decode ourselves via StringDecoder for safe multi-byte splits
    });

    this.delimiter = options.delimiter || ',';
    this.quote = options.quote || '"';
    this.skipEmptyLines = options.skipEmptyLines !== false;

    this._decoder = new StringDecoder('utf8');

    // parser state
    this._buffer = '';       // unparsed remainder across chunks
    this._row = [];          // fields accumulated for the current row
    this._field = '';        // current field text
    this._fieldStarted = false; // true once the current field has begun (quote or char)
    this._inQuotes = false;  // true while scanning inside a quoted field
    this._rowStarted = false;   // true once the current row has real content
  }

  // -- Transform hooks -------------------------------------------------------

  _transform(chunk, _encoding, callback) {
    this._buffer += Buffer.isBuffer(chunk)
      ? this._decoder.write(chunk)
      : String(chunk);
    this._parse(false);
    callback();
  }

  _flush(callback) {
    this._buffer += this._decoder.end();
    this._parse(true);

    // Emit a trailing row if the input did not end with a newline.
    if (this._rowStarted || this._field !== '' || this._fieldStarted) {
      this._row.push(this._field);
      this._emitRow();
    }
    callback();
  }

  // -- internal helpers ------------------------------------------------------

  _emitRow() {
    if (this._rowStarted || !this.skipEmptyLines) {
      this.push(this._row);
    }
    this._row = [];
    this._field = '';
    this._fieldStarted = false;
    this._inQuotes = false;
    this._rowStarted = false;
  }

  /**
   * Consume as many complete tokens as possible from _buffer.
   * When `flush` is true we are at end-of-stream: don't wait for lookahead.
   */
  _parse(flush) {
    const buf = this._buffer;
    const len = buf.length;
    let i = 0;

    while (i < len) {
      const ch = buf[i];

      // ---- inside a quoted field ------------------------------------------
      if (this._inQuotes) {
        if (ch === this.quote) {
          if (i + 1 < len) {
            if (buf[i + 1] === this.quote) {
              // doubled quote → literal quote in the field
              this._field += this.quote;
              i += 2;
              continue;
            }
            // closing quote
            this._inQuotes = false;
            i++;
            continue;
          }
          // quote is the last char in the buffer — need lookahead
          if (flush) {
            this._inQuotes = false;
            i++;
            continue;
          }
          break; // wait for the next chunk to decide escaped vs. closing
        }

        // any other character (including newlines and the delimiter) is literal
        this._field += ch;
        i++;
        continue;
      }

      // ---- outside quotes -------------------------------------------------

      // Start of a fresh field.
      if (!this._fieldStarted) {
        if (ch === this.quote) {
          this._inQuotes = true;
          this._fieldStarted = true;
          this._rowStarted = true;
          i++;
          continue;
        }
        this._fieldStarted = true;
        this._rowStarted = true;
      }

      // Field delimiter → close the field.
      if (ch === this.delimiter) {
        this._row.push(this._field);
        this._field = '';
        this._fieldStarted = false;
        i++;
        continue;
      }

      // Carriage return → CRLF, lone CR, or needs lookahead.
      if (ch === '\r') {
        if (i + 1 < len) {
          if (buf[i + 1] === '\n') {
            this._row.push(this._field);
            this._emitRow();
            i += 2;
            continue;
          }
          // lone CR counts as a line break
          this._row.push(this._field);
          this._emitRow();
          i++;
          continue;
        }
        if (flush) {
          this._row.push(this._field);
          this._emitRow();
          i++;
          continue;
        }
        break; // wait to see if a \n follows
      }

      // LF line break.
      if (ch === '\n') {
        this._row.push(this._field);
        this._emitRow();
        i++;
        continue;
      }

      // Normal character — append to the current field.
      this._field += ch;
      i++;
    }

    this._buffer = buf.slice(i);
  }
}

/**
 * Parse a string or Buffer into an array of rows (arrays of strings).
 *
 * @param {string|Buffer} input
 * @param {object} [options]  Same options as CSVParser.
 * @returns {Promise<string[][]>}
 */
function parseAll(input, options = {}) {
  return new Promise((resolve, reject) => {
    const parser = new CSVParser(options);
    const rows = [];
    parser.on('data', (row) => rows.push(row));
    parser.on('end', () => resolve(rows));
    parser.on('error', reject);
    parser.write(input);
    parser.end();
  });
}

module.exports = { CSVParser, parseAll };

// ---------------------------------------------------------------------------
// Demo: parse a CSV string and print each row.
// ---------------------------------------------------------------------------
if (require.main === module) {
  const data = [
    'name,age,note',
    '"John ""JD"" Doe",30,"hello, world"',
    'Jane,25,"spans\ntwo lines"',
    '"Bob ""The Man""",40,"embedded ""quotes"" and, commas"',
    '',
    '"tail",99,"no trailing newline"',
  ].join('\n');

  console.log('--- CSV input ---');
  console.log(data);
  console.log('\n--- parsed rows ---');

  parseAll(data).then((rows) => {
    for (const row of rows) {
      console.log(row);
    }
  });
}
