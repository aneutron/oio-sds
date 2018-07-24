// OpenIO SDS Go rawx
// Copyright (C) 2015-2018 OpenIO SAS
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 3.0 of the License, or (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public
// License along with this program. If not, see <http://www.gnu.org/licenses/>.

package main

import (
	"bytes"
	"compress/zlib"
	"crypto/md5"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"io/ioutil"
	"net/http"
	"path/filepath"
	"strings"
)

const bufSize = 1024 * 1024

type attrMapping struct {
	attr   string
	header string
}

const (
	AttrNameChecksum     = "user.grid.chunk.hash"
	AttrNamePosition     = "user.grid.chunk.position"
	AttrNameSize         = "user.grid.chunk.size"
	AttrNameChunkId      = "user.grid.chunk.id"
	AttrNameChunkMethod  = "user.grid.content.chunk_method"
	AttrNameMimeType     = "user.grid.content.mime_type"
	AttrNameStgPol       = "user.grid.content.storage_policy"
	AttrNameAlias        = "user.grid.content"
	AttrNameCompression  = "user.grid.compression"
	AttrNameXattrVersion = "user.grid.oio.version"
)

const (
	AttrNameFullPrefix = "user.grid.oio:"
)

const (
	HeaderNameAlias             = "X-oio-Alias"
	HeaderNameStgPol            = "X-oio-Chunk-Meta-Content-Storage-Policy"
	HeaderNameMimeType          = "X-oio-Chunk-Meta-Content-Mime-Type"
	HeaderNameChunkMethod       = "X-oio-Chunk-Meta-Content-Chunk-Method"
	HeaderNameChunkId           = "X-oio-Chunk-Meta-Chunk-Id"
	HeaderNamePosition          = "X-oio-Chunk-Meta-Chunk-Pos"
	HeaderNameSize              = "X-oio-Chunk-Meta-Chunk-Size"
	HeaderNameChecksum          = "X-oio-Chunk-Meta-Chunk-Hash"
	HeaderNameMetachunkSize     = "X-oio-Chunk-Meta-Metachunk-Size"
	HeaderNameMetachunkChecksum = "X-oio-Chunk-Meta-Metachunk-Hash"
	HeaderNameFullPath          = "X-oio-Chunk-Meta-Full-Path"
)

var (
	AttrValueZLib []byte = []byte{'z', 'l', 'i', 'b'}
)

var (
	ErrNotImplemented        = errors.New("Not implemented")
	ErrChunkExists           = errors.New("Chunk already exists")
	ErrInvalidChunkName      = errors.New("Invalid chunk name")
	ErrCompressionNotManaged = errors.New("Compression mode not managed")
	ErrMissingHeader         = errors.New("Missing mandatory header")
	ErrMd5Mismatch           = errors.New("MD5 sum mismatch")
	ErrInvalidRange          = errors.New("Invalid range")
	ErrRangeNotSatisfiable   = errors.New("Range not satisfiable")
	ErrListMarker            = errors.New("Invalid listing marker")
	ErrListPrefix            = errors.New("Invalid listing prefix")
)

var AttrMap []attrMapping = []attrMapping{
	{AttrNameAlias, HeaderNameAlias},
	{AttrNameStgPol, HeaderNameStgPol},
	{AttrNameMimeType, HeaderNameMimeType},
	{AttrNameChunkMethod, HeaderNameChunkMethod},
	{AttrNameChunkId, HeaderNameChunkId},
	{AttrNameSize, HeaderNameSize},
	{AttrNamePosition, HeaderNamePosition},
	{AttrNameChecksum, HeaderNameChecksum},
}

var mandatoryHeaders = []string{
	AttrNameStgPol,
	AttrNameChunkMethod,
	AttrNameSize,
	AttrNamePosition,
}

type upload struct {
	in     io.Reader
	length int64
	h      string
}

func putData(out io.Writer, ul *upload) error {
	running := true
	remaining := ul.length
	logger_error.Printf("Uploading %v bytes", remaining)
	chunkHash := md5.New()
	buf := make([]byte, bufSize)
	for running && remaining != 0 {
		max := int64(bufSize)
		if remaining > 0 && remaining < bufSize {
			max = remaining
		}
		n, err := ul.in.Read(buf[:max])
		logger_error.Printf("consumed %v / %s", n, err)
		if n > 0 {
			if remaining > 0 {
				remaining = remaining - int64(n)
			}
			out.Write(buf[:n])
			chunkHash.Write(buf[:n])
		}
		if err != nil {
			if err == io.EOF && remaining < 0 {
				// Clean end of chunked stream
				running = false
			} else {
				// Any other error
				return err
			}
		}
	}

	sum := chunkHash.Sum(make([]byte, 0))
	ul.h = strings.ToUpper(hex.EncodeToString(sum))
	return nil
}

func putFinishChecksum(rr *rawxRequest, h string) error {

	h = strings.ToUpper(h)
	if h0, ok := rr.xattr[AttrNameChecksum]; ok && len(h0) > 0 {
		if strings.ToUpper(h0) != h {
			return ErrMd5Mismatch
		}
	} else {
		rr.xattr[AttrNameChecksum] = h
	}

	return nil
}

func putFinishXattr(rr *rawxRequest, out FileWriter) error {

	logger_error.Printf("Decorating with xattr: %v", rr.xattr)
	for k, v := range rr.xattr {
		if err := out.SetAttr(k, []byte(v)); err != nil {
			return err
		}
	}

	return nil
}

func uploadChunk(rr *rawxRequest, chunkid string) {

	// Load the upload-related headers
	for _, pair := range AttrMap {
		if v := rr.req.Header.Get(pair.header); v != "" {
			rr.xattr[pair.attr] = v
		}
	}

	// Check all the mandatory headers are present
	for _, k := range mandatoryHeaders {
		if _, ok := rr.xattr[k]; !ok {
			logger_error.Print("Missing header: ", k)
			rr.replyError(ErrMissingHeader)
			return
		}
	}

	// Attempt a PUT in the repository
	out, err := rr.rawx.repo.Put(chunkid)
	if err != nil {
		logger_error.Print("Chunk opening error: ", err)
		rr.replyError(err)
		return
	}

	// Upload, and maybe manage compression
	var ul upload
	ul.in = rr.req.Body
	ul.length = rr.req.ContentLength

	if rr.rawx.compress {
		z := zlib.NewWriter(out)
		err = putData(z, &ul)
		errClose := z.Close()
		if err == nil {
			err = errClose
		}
	} else {
		if err = putData(out, &ul); err != nil {
			logger_error.Print("Chunk upload error: ", err)
		}
	}

	// If a hash has been sent, it must match the hash computed
	if err == nil {
		if err = putFinishChecksum(rr, ul.h); err != nil {
			logger_error.Print("Chunk checksum error: ", err)
		}
	}

	// If everything went well, finish with the chunks XATTR management
	if err == nil {
		if err = putFinishXattr(rr, out); err != nil {
			logger_error.Print("Chunk xattr error: ", err)
		}
	}

	// Then reply
	if err != nil {
		rr.replyError(err)
		out.Abort()
	} else {
		out.Commit()
		rr.rep.Header().Set("chunkhash", ul.h)
		rr.replyCode(http.StatusCreated)
	}
}

func checkChunk(rr *rawxRequest, chunkid string) {
	in, err := rr.rawx.repo.Get(chunkid)
	if in != nil {
		defer in.Close()
	}

	length := in.Size()
	rr.rep.Header().Set("Content-Length", fmt.Sprintf("%v", length))
	rr.rep.Header().Set("Accept-Ranges", "bytes")

	if err != nil {
		rr.replyError(err)
	} else {
		rr.replyCode(http.StatusNoContent)
	}
}

func downloadChunk(rr *rawxRequest, chunkid string) {
	inChunk, err := rr.rawx.repo.Get(chunkid)
	if inChunk != nil {
		defer inChunk.Close()
	}
	if err != nil {
		logger_error.Print("File error: ", err)
		rr.replyError(err)
		return
	}

	hdr_range := rr.req.Header.Get("Range")
	var offset, size int64
	if len(hdr_range) > 0 {
		var nb int
		var last int64
		nb, err := fmt.Fscanf(strings.NewReader(hdr_range), "bytes=%d-%d", &offset, &last)
		if err != nil || nb != 2 || last <= offset {
			rr.replyError(ErrInvalidRange)
			return
		}
		size = last - offset + 1
	}

	has_range := func() bool {
		return len(hdr_range) > 0
	}

	// Check if there is some compression
	var v []byte
	var in io.ReadCloser
	v, err = inChunk.GetAttr(AttrNameCompression)
	if err != nil {
		if has_range() && offset > 0 {
			err = inChunk.Seek(offset)
		} else {
			in = ioutil.NopCloser(inChunk)
			err = nil
		}
	} else if bytes.Equal(v, AttrValueZLib) {
		//in, err = zlib.NewReader(in)
		// TODO(jfs): manage the Range offset
		err = ErrCompressionNotManaged
	} else {
		err = ErrCompressionNotManaged
	}

	if in != nil {
		defer in.Close()
	}
	if err != nil {
		setError(rr.rep, err)
		rr.replyCode(http.StatusInternalServerError)
		return
	}

	// If the range specified a size, let's wrap (again) the input
	if has_range() && size > 0 {
		in = &limitedReader{sub: in, remaining: size}
	}

	for _, pair := range AttrMap {
		v, err := inChunk.GetAttr(pair.attr)
		if err != nil {
			rr.rep.Header().Set(pair.header, string(v))
		}
	}

	// Prepare the headers of the reply
	if has_range() {
		rr.rep.Header().Set("Content-Range", fmt.Sprintf("bytes %v-%v/%v", offset, offset+size, size))
		rr.rep.Header().Set("Content-Length", fmt.Sprintf("%v", size))
		if size <= 0 {
			rr.replyCode(http.StatusNoContent)
		} else {
			rr.replyCode(http.StatusPartialContent)
		}
	} else {
		length := inChunk.Size()
		rr.rep.Header().Set("Content-Length", fmt.Sprintf("%v", length))
		if length <= 0 {
			rr.replyCode(http.StatusNoContent)
		} else {
			rr.replyCode(http.StatusOK)
		}
	}

	// Now transmit the clear data to the client
	buf := make([]byte, bufSize)
	for {
		n, err := in.Read(buf)
		if n > 0 {
			rr.bytes_out = rr.bytes_out + uint64(n)
			rr.rep.Write(buf[:n])
		}
		if err != nil {
			if err != io.EOF {
				logger_error.Print("Write() error: ", err)
			}
			break
		}
	}
}

func removeChunk(rr *rawxRequest, chunkid string) {
	if err := rr.rawx.repo.Del(chunkid); err != nil {
		rr.replyError(err)
	} else {
		rr.replyCode(http.StatusNoContent)
	}
}

func (rr *rawxRequest) serveChunk(rep http.ResponseWriter, req *http.Request) {
	chunkid := filepath.Base(req.URL.Path)
	switch req.Method {
	case "PUT":
		rr.stats_time = TimePut
		rr.stats_hits = HitsPut
		uploadChunk(rr, chunkid)
	case "HEAD":
		rr.stats_time = TimeHead
		rr.stats_hits = HitsHead
		checkChunk(rr, chunkid)
	case "GET":
		rr.stats_time = TimeGet
		rr.stats_hits = HitsGet
		downloadChunk(rr, chunkid)
	case "DELETE":
		rr.stats_time = TimeDel
		rr.stats_hits = HitsDel
		removeChunk(rr, chunkid)
	default:
		rr.replyCode(http.StatusMethodNotAllowed)
	}
}
