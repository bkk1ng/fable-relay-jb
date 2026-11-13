'use strict';

/**
 * csv.js — streaming CSV parser for Node.js 22 (CommonJS, no dependencies)
 *
 * Handles:
 *   - Quoted fields (RFC 4180)
 *   - Escaped quotes ("")
 *   - Embedded newlines inside quoted fields
 *   - CRLF and LF line endings
 *   - Configurable delimiter (default: comma)
 *
 * Exports:
 *   CsvParser  — Transform stream, emits one string[] per data row
 *   parseAll   — convenience: string → string[][]
 */

const { Transform } = require('stream');

// ─── parser state machine ────────────────────────────────────────────────────

const STATE_FIELD_START  = 0;  // beginning of a new field
const STATE_UNQUOTED     = 1;  // inside an unquoted field
const STATE_QUOTED       = 2;  // inside a quoted field
const STATE_QUOTE_SEEN   = 3;  // just saw a closing/escaping " inside a quoted field

/**
 * Low-level incremental parser.  Feed it chunks, collect rows.
 *
 * @param {object}   opts
 * @param {string}   opts.delimiter   single-character field separator (default ',')
 * @param {string[]} [opts.row]       partial row carried across chunks
 * @param {string}   [opts.field]     partial field carried across chunks
 * @param {number}   [opts.state]     parser state carried across chunks
 */
function makeParserState(opts = {}) {
  return {
    delimiter : (opts.delimiter ?? ',')[0],
    row       : opts.row   ?? [],
    field     : opts.field ?? '',
    state     : opts.state ?? STATE_FIELD_START,
  };
}

/**
 * Process a chunk of text, appending complete rows to `out`.
 * Mutates `ps` (parser state) in place.
 *
 * @param {object}   ps    parser state from makeParserState()
 * @param {string}   chunk text to process
 * @param {string[][]} out  collector — complete rows are pushed here
 */
function processChunk(ps, chunk, out) {
  const { delimiter } = ps;
  const len = chunk.length;
  let i = 0;

  while (i < len) {
    const ch = chunk[i];

    switch (ps.state) {

      case STATE_FIELD_START:
        if (ch === '"') {
          ps.state = STATE_QUOTED;
          i++;
        } else if (ch === delimiter) {
          // empty unquoted field
          ps.row.push('');
          ps.state = STATE_FIELD_START;
          i++;
        } else if (ch === '\r') {
          // might be CRLF; peek ahead
          if (i + 1 < len && chunk[i + 1] === '\n') {
            i++;  // skip the \r, \n handled next iteration
          } else {
            // bare \r — treat as row end
            ps.row.push(ps.field);
            ps.field = '';
            out.push(ps.row);
            ps.row = [];
            ps.state = STATE_FIELD_START;
            i++;
          }
        } else if (ch === '\n') {
          ps.row.push(ps.field);
          ps.field = '';
          out.push(ps.row);
          ps.row = [];
          ps.state = STATE_FIELD_START;
          i++;
        } else {
          ps.state = STATE_UNQUOTED;
          ps.field += ch;
          i++;
        }
        break;

      case STATE_UNQUOTED:
        if (ch === delimiter) {
          ps.row.push(ps.field);
          ps.field = '';
          ps.state = STATE_FIELD_START;
          i++;
        } else if (ch === '\r') {
          if (i + 1 < len && chunk[i + 1] === '\n') {
            i++;  // consume \r, fall through to \n on next iteration
          } else {
            ps.row.push(ps.field);
            ps.field = '';
            out.push(ps.row);
            ps.row = [];
            ps.state = STATE_FIELD_START;
            i++;
          }
        } else if (ch === '\n') {
          ps.row.push(ps.field);
          ps.field = '';
          out.push(ps.row);
          ps.row = [];
          ps.state = STATE_FIELD_START;
          i++;
        } else {
          ps.field += ch;
          i++;
        }
        break;

      case STATE_QUOTED:
        if (ch === '"') {
          ps.state = STATE_QUOTE_SEEN;
          i++;
        } else {
          // newlines (including CRLF) are allowed verbatim inside quoted fields
          if (ch === '\r' && i + 1 < len && chunk[i + 1] === '\n') {
            ps.field += '\r\n';
            i += 2;
          } else {
            ps.field += ch;
            i++;
          }
        }
        break;

      case STATE_QUOTE_SEEN:
        if (ch === '"') {
          // escaped quote: "" → "
          ps.field += '"';
          ps.state = STATE_QUOTED;
          i++;
        } else if (ch === delimiter) {
          // end of quoted field
          ps.row.push(ps.field);
          ps.field = '';
          ps.state = STATE_FIELD_START;
          i++;
        } else if (ch === '\r') {
          if (i + 1 < len && chunk[i + 1] === '\n') {
            i++;  // consume \r
          } else {
            ps.row.push(ps.field);
            ps.field = '';
            out.push(ps.row);
            ps.row = [];
            ps.state = STATE_FIELD_START;
            i++;
          }
        } else if (ch === '\n') {
          ps.row.push(ps.field);
          ps.field = '';
          out.push(ps.row);
          ps.row = [];
          ps.state = STATE_FIELD_START;
          i++;
        } else {
          // non-standard content after closing quote — absorb it
          ps.field += ch;
          ps.state = STATE_UNQUOTED;
          i++;
        }
        break;

      default:
        i++;
    }
  }
}

/**
 * Flush any trailing partial row that wasn't terminated by a newline.
 * Returns the final row, or null if there was nothing pending.
 *
 * @param {object} ps  parser state
 * @returns {string[]|null}
 */
function flushParser(ps) {
  // A completely empty parser state means the input ended cleanly.
  if (ps.row.length === 0 && ps.field === '' && ps.state === STATE_FIELD_START) {
    return null;
  }
  const finalRow = ps.row.concat(ps.field);
  ps.row = [];
  ps.field = '';
  ps.state = STATE_FIELD_START;
  return finalRow;
}

// ─── Transform stream ────────────────────────────────────────────────────────

/**
 * CsvParser — a Transform stream that accepts text (or Buffers) and emits
 * one string[] per complete CSV row.
 *
 * @example
 * const parser = new CsvParser({ delimiter: '\t' });
 * fs.createReadStream('data.tsv').pipe(parser);
 * parser.on('data', row => console.log(row));
 */
class CsvParser extends Transform {
  /**
   * @param {object} [opts]
   * @param {string} [opts.delimiter=',']  field separator character
   * @param {object} [opts.streamOpts]     passed to Transform constructor
   */
  constructor(opts = {}) {
    const { delimiter = ',', ...streamOpts } = opts;
    super({ ...streamOpts, readableObjectMode: true });
    this._ps = makeParserState({ delimiter });
  }

  _transform(chunk, encoding, callback) {
    const text = Buffer.isBuffer(chunk) ? chunk.toString('utf8') : chunk;
    const rows = [];
    try {
      processChunk(this._ps, text, rows);
    } catch (err) {
      return callback(err);
    }
    for (const row of rows) this.push(row);
    callback();
  }

  _flush(callback) {
    const last = flushParser(this._ps);
    if (last !== null) this.push(last);
    callback();
  }
}

// ─── parseAll convenience ────────────────────────────────────────────────────

/**
 * Parse an entire CSV string synchronously.
 *
 * @param {string} text        full CSV content
 * @param {object} [opts]
 * @param {string} [opts.delimiter=',']
 * @returns {string[][]}       array of rows, each row an array of field strings
 */
function parseAll(text, opts = {}) {
  const ps = makeParserState({ delimiter: opts.delimiter ?? ',' });
  const rows = [];
  processChunk(ps, text, rows);
  const last = flushParser(ps);
  if (last !== null) rows.push(last);
  return rows;
}

// ─── demo ────────────────────────────────────────────────────────────────────

function demo() {
  const csv = [
    'id,name,note',
    '1,Alice,"quiet, careful"',
    '2,Bob,"line one\r\nline two"',
    '3,"Charlie ""Chuck"" Davis",plain',
    '4,Eve,',
  ].join('\r\n');

  console.log('=== parseAll demo ===');
  const rows = parseAll(csv);
  rows.forEach((row, i) => console.log(`row ${i}:`, row));

  console.log('\n=== Transform stream demo ===');
  const { Readable } = require('stream');

  // Feed the same content in small arbitrary chunks to prove streaming works.
  const CHUNK = 20;
  const parser = new CsvParser();
  let rowIndex = 0;

  parser.on('data', row => console.log(`row ${rowIndex++}:`, row));
  parser.on('end',  () => console.log('stream finished'));

  const source = new Readable({ read() {} });
  source.pipe(parser);

  for (let i = 0; i < csv.length; i += CHUNK) {
    source.push(csv.slice(i, i + CHUNK));
  }
  source.push(null);  // signal EOF
}

demo();

// ─── exports ─────────────────────────────────────────────────────────────────

module.exports = { CsvParser, parseAll };
