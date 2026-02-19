import { requireHostFunction } from 'sage:internal/host'

const dataDirHost = requireHostFunction('__sage_fs_data_dir')
const existsHost = requireHostFunction('__sage_fs_exists')
const readTextHost = requireHostFunction('__sage_fs_read_text')
const readBytesHost = requireHostFunction('__sage_fs_read_bytes')
const readDataTextHost = requireHostFunction('__sage_fs_read_data_text')
const readDataBytesHost = requireHostFunction('__sage_fs_read_data_bytes')
const writeDataTextHost = requireHostFunction('__sage_fs_write_data_text')
const writeDataBytesHost = requireHostFunction('__sage_fs_write_data_bytes')
const listDataHost = requireHostFunction('__sage_fs_list_data')

function parseReadOpts(opts) {
  let encoding = null
  let maxBytes = undefined
  if (typeof opts === 'string') {
    encoding = opts
  } else if (opts && typeof opts === 'object') {
    if (typeof opts.encoding === 'string') encoding = opts.encoding
    if (typeof opts.maxBytes === 'number') maxBytes = opts.maxBytes
  }
  if (encoding) encoding = String(encoding).toLowerCase()
  return { encoding, maxBytes }
}

export function dataDir() {
  return String(dataDirHost())
}

export async function exists(path) {
  return existsHost(String(path)) === true
}

export async function readFile(path, opts) {
  const o = parseReadOpts(opts)
  const p = String(path)
  if (o.encoding) {
    if (o.encoding !== 'utf8' && o.encoding !== 'utf-8') {
      throw new Error('sage:fs.readFile: unsupported encoding: ' + o.encoding)
    }
    return readTextHost(p, o.maxBytes)
  }
  const ab = readBytesHost(p, o.maxBytes)
  return new Uint8Array(ab)
}

export async function readDataFile(name, opts) {
  const o = parseReadOpts(opts)
  const n = String(name)
  if (o.encoding) {
    if (o.encoding !== 'utf8' && o.encoding !== 'utf-8') {
      throw new Error('sage:fs.readDataFile: unsupported encoding: ' + o.encoding)
    }
    return readDataTextHost(n, o.maxBytes)
  }
  const ab = readDataBytesHost(n, o.maxBytes)
  return new Uint8Array(ab)
}

function toBytes(data) {
  if (typeof data === 'string') {
    return { kind: 'text', text: data }
  }
  if (data instanceof Uint8Array) {
    return { kind: 'bytes', bytes: data }
  }
  if (data && typeof ArrayBuffer !== 'undefined' && data instanceof ArrayBuffer) {
    return { kind: 'bytes', bytes: new Uint8Array(data) }
  }
  throw new TypeError('sage:fs: expected string or Uint8Array')
}

async function writeData(name, data, append) {
  const n = String(name)
  const v = toBytes(data)
  const ok =
    v.kind === 'text'
      ? writeDataTextHost(n, v.text, !!append) === 0
      : writeDataBytesHost(n, v.bytes, !!append) === 0
  if (!ok) {
    throw new Error('sage:fs: write failed')
  }
}

export async function writeFile(name, data) {
  return writeData(name, data, false)
}

export async function appendFile(name, data) {
  return writeData(name, data, true)
}

export async function readdir() {
  return listDataHost()
}

export default Object.freeze({
  dataDir,
  exists,
  readFile,
  readDataFile,
  writeFile,
  appendFile,
  readdir,
})
