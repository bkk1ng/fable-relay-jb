'use strict';

/**
 * csv.js — streaming CSV parser for Node.js (CommonJS).
 *
 * Handles quoted fields, escaped quotes (""), embedded newlines, CRLF/LF/CR line endings,
 * and a configurable field delimiter. Exposes:
 *   - CsvParser: a Transform stream that emits one row at a time (string[]).
 *   - parseAll(input, options): convenience that parses a full string/Buffer and returns string[][].
 *
 * No external dependencies. Node >= 12 (uses stream.Transform, Buffer).
 */

const { Transform } = require('stream');

/**
 * Transform stream that parses CSV data incrementally.
 *
 * Options:
 *   - delimiter (string, default ","): single-character field delimiter.
 *   - quote (string, default '"'): quote character wrapping fields that contain
 *     delimiters, newlines, or the quote character itself. Inside a quoted field,
 *     two consecutive quote characters are treated as a literal quote.
 *   - objectMode (boolean, default true): when true, pushes string[] rows directly;
 *     when false, pushes each row as a JSON-encoded line (string).
 *   - emitHeader (boolean, default false): if true, the first emitted row is also
 *     accessible via the `header` property after it has been parsed.
 *
 * Events:
 *   - 'data' (row): string[] of field values for one row.
 *   - 'end': all rows emitted.
 *
 * Properties:
 *   - header: string[] | null — the first parsed row (when emitHeader is true), else null.
 */
class CsvParser extends Transform {
  constructor(options = {}) {
    const {
      delimiter = ',',
      quote = '"',
      objectMode = true,
      emitHeader = false,
      ...transformOpts
    } = options;

    if (typeof delimiter !== 'string' || delimiter.length !== 1) {
      throw new TypeError('delimiter must be a single character string');
    }
    if (typeof quote !== 'string' || quote.length !== 1) {
      throw new TypeError('quote must be a single character string');
    }
    if (delimiter === quote) {
      throw new TypeError('delimiter and quote must be different characters');
    }

    super({ ...transformOpts, objectMode });

    this.delimiter = delimiter;
    this.quote = quote;
    this.emitHeader = emitHeader;

    // Parsing state machine.
    // state: 'field-start' | 'in-unquoted' | 'in-quoted' | 'quote-in-quoted' | 'after-quoted'
    this.buffer = '';        // undecoded text pending parse
    this.row = [];           // fields of the current row being built
    this.field = '';         // current field text being accumulated
    this.state = 'field-start';
    this.header = null;
    this._rowIndex = 0;
  }

  /**
   * Push a complete row. When emitHeader is set, the first row becomes this.header.
   * @param {string[]} row
   * @private
   */
  _pushRow(row) {
    if (this.emitHeader && this._rowIndex === 0) {
      this.header = row;
    }
    this._rowIndex++;
    if (this.objectMode) {
      this.push(row);
    } else {
      this.push(JSON.stringify(row) + '\n');
    }
  }

  /**
   * Consume as many complete fields/rows as are present in this.buffer.
   * Leaves any trailing partial field in this.buffer for the next chunk.
   * @private
   */
  _consume() {
    let i = 0;
    const len = this.buffer.length;

    while (i < len) {
      const ch = this.buffer[i];

      switch (this.state) {
        case 'field-start': {
          // Beginning of a new field. A quote here means a quoted field.
          if (ch === this.quote) {
            this.state = 'in-quoted';
            i++;
          } else if (ch === this.delimiter) {
            // Empty unquoted field, immediately followed by a delimiter.
            this.row.push(this.field);
            this.field = '';
            i++;
          } else if (ch === '\r') {
            // Possible CRLF; peek ahead. Treat lone CR as a row terminator too.
            if (this.buffer[i + 1] === '\n') {
              this.row.push(this.field);
              this.field = '';
              this.row = [];
              i += 2;
            } else {
              this.row.push(this.field);
              this.field = '';
              this.row = [];
              i++;
            }
          } else if (ch === '\n') {
            this.row.push(this.field);
            this.field = '';
            this.row = [];
            i++;
          } else {
            this.field += ch;
            this.state = 'in-unquoted';
            i++;
          }
          break;
        }

        case 'in-unquoted': {
          // Reading an unquoted field until delimiter or newline.
          if (ch === this.delimiter) {
            this.row.push(this.field);
            this.field = '';
            this.state = 'field-start';
            i++;
          } else if (ch === '\r') {
            if (this.buffer[i + 1] === '\n') {
              this.row.push(this.field);
              this.field = '';
              this._pushRow(this.row);
              this.row = [];
              this.state = 'field-start';
              i += 2;
            } else {
              this.row.push(this.field);
              this.field = '';
              this._pushRow(this.row);
              this.row = [];
              this.state = 'field-start';
              i++;
            }
          } else if (ch === '\n') {
            this.row.push(this.field);
            this.field = '';
            this._pushRow(this.row);
            this.row = [];
            this.state = 'field-start';
            i++;
          } else {
            this.field += ch;
            i++;
          }
          break;
        }

        case 'in-quoted': {
          // Inside a quoted field. A quote is either an escape ("") or the closing quote.
          if (ch === this.quote) {
            if (this.buffer[i + 1] === this.quote) {
              // Escaped quote: literal quote character.
              this.field += this.quote;
              i += 2;
            } else {
              // Closing quote.
              this.state = 'after-quoted';
              i++;
            }
          } else {
            this.field += ch;
            i++;
          }
          break;
        }

        case 'after-quoted': {
          // The closing quote has been read. We now expect a delimiter, newline, or EOF.
          if (ch === this.delimiter) {
            this.row.push(this.field);
            this.field = '';
            this.state = 'field-start';
            i++;
          } else if (ch === '\r') {
            if (this.buffer[i + 1] === '\n') {
              this.row.push(this.field);
              this.field = '';
              this._pushRow(this.row);
              this.row = [];
              this.state = 'field-start';
              i += 2;
            } else {
              this.row.push(this.field);
              this.field = '';
              this._pushRow(this.row);
              this.row = [];
              this.state = 'field-start';
              i++;
            }
          } else if (ch === '\n') {
            this.row.push(this.field);
            this.field = '';
            this._pushRow(this.row);
            this.row = [];
            this.state = 'field-start';
            i++;
          } else {
            // Stray characters after a closing quote. Per RFC 4180 this is malformed,
            // but we tolerate it by appending to the field as if unquoted.
            this.state = 'in-unquoted';
            // Do not consume ch; let the in-unquoted case handle it next loop.
          }
          break;
        }

        default:
          i++;
          break;
      }
    }

    // Retain the unprocessed tail. In 'in-quoted' we may have stopped mid-escape
    // (a quote with no following char yet); the state will handle it next chunk.
    this.buffer = this.buffer.slice(i);
  }

  /**
   * Transform implementation: append chunk to buffer and consume complete tokens.
   * @param {Buffer|string} chunk
   * @param {string} encoding
   * @param {Function} callback
   * @private
   */
  _transform(chunk, encoding, callback) {
    if (Buffer.isBuffer(chunk)) {
      this.buffer += chunk.toString('utf8');
    } else {
      this.buffer += chunk;
    }
    try {
      this._consume();
      callback();
    } catch (err) {
      callback(err);
    }
  }

  /**
   * Flush any final partial row. Handles the case where the input does not end
   * with a trailing newline, or where a quoted field was left open (we still emit
   * what we have rather than dropping the last row).
   * @param {Function} callback
   * @private
   */
  _flush(callback) {
    // Finish any trailing field/row not terminated by a newline.
    if (this.state === 'in-quoted') {
      // Unterminated quoted field: emit the accumulated text as-is.
      this.row.push(this.field);
      this.field = '';
      this._pushRow(this.row);
      this.row = [];
    } else if (this.state === 'after-quoted' || this.state === 'in-unquoted' || this.state === 'field-start') {
      // A field (possibly empty) has been accumulated without a trailing delimiter/newline.
      // Only emit if the row is non-empty OR the field is non-empty OR we already have fields.
      // This avoids emitting a spurious empty row from a trailing newline.
      if (this.field !== '' || this.row.length > 0) {
        this.row.push(this.field);
        this.field = '';
        this._pushRow(this.row);
        this.row = [];
      }
    }
    this.buffer = '';
    callback();
  }
}

/**
 * Convenience: parse an entire CSV string/Buffer synchronously and return all rows.
 *
 * @param {string|Buffer} input
 * @param {Object} [options] — same options as CsvParser (objectMode is forced off here
 *   since we collect rows directly).
 * @returns {string[][]}
 */
function parseAll(input, options = {}) {
  const { delimiter, quote, emitHeader } = options;
  const parser = new CsvParser({
    delimiter,
    quote,
    emitHeader,
    objectMode: true,
    // readableObjectMode so push() accepts arrays.
    readableObjectMode: true,
    writableObjectMode: true,
  });

  const rows = [];
  parser.on('data', (row) => rows.push(row));
  parser.on('error', (err) => { throw err; });

  // end() triggers _flush synchronously within the same tick of the event loop
  // for a fresh Transform with no I/O scheduling, but to be safe we return after
  // the 'end' event.
  let ended = false;
  parser.on('end', () => { ended = true; });

  parser.write(input);
  parser.end();

  // For a non-IO-bound Transform, 'end' fires synchronously after end() returns.
  // If for some reason it did not (unlikely), fall back to a sync drain.
  if (!ended) {
    // Defensive: this branch should not normally trigger for in-memory parsing.
    // We drain by reading until null. In Node, a non-flowing Transform with no
    // 'data' listener removal would need resume(); here rows already captured.
    parser.resume();
  }

  return rows;
}

// ---------------------------------------------------------------------------
// Demo: read CSV from a string and print each row. Runs when invoked directly.
// ---------------------------------------------------------------------------
function demo() {
  const csv = [
    'name,role,quote',
    'Alice,admin,"She said, ""hi"" and left"',
    'Bob,"editor\nwriter","multi-line\nfield"',
    'Carol,member,plain text\r\n',
    'Dave,guest,"trailing quote"',
  ].join('\n');

  console.log('--- parseAll demo ---');
  const rows = parseAll(csv, { emitHeader: true });
  console.log('Header:', rows[0]);
  for (let i = 1; i < rows.length; i++) {
    console.log(`Row ${i}:`, rows[i]);
  }

  console.log('\n--- streaming demo ---');
  const parser = new CsvParser({ emitHeader: true });
  parser.on('data', (row) => console.log('stream row:', row));
  parser.on('end', () => console.log('stream end. header =', parser.header));
  parser.on('error', (err) => console.error('stream error:', err));

  // Feed the same CSV in two chunks to demonstrate streaming.
  const mid = Math.floor(csv.length / 2);
  parser.write(csv.slice(0, mid));
  parser.write(csv.slice(mid));
  parser.end();
}

module.exports = CsvParser;
module.exports.CsvParser = CsvParser;
module.exports.parseAll = parseAll;

if (require.main === module) {
  demo();
}
