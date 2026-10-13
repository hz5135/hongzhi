package cli

import (
	"archive/tar"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"

	"agentbench/sandbox/internal/sbx"
)

// cmdUpload implements `sbx upload [sandbox] -- <file>... [-d dest]`.
func cmdUpload(args []string) int {
	ep := &agentEndpoint{port: 9119}
	if !resolveAgent(args, 1, ep) {
		return 1
	}

	dest := "/root"
	var files []string
	for k := 1; k < len(args); k++ {
		arg := args[k]
		switch arg {
		case "-d", "--dest":
			if k+1 < len(args) {
				k++
				dest = args[k]
			}
			continue
		case "--host", "--port", "--token":
			/* resolveAgent's endpoint options: skip the value too, it is not
			   an upload target */
			k++
			continue
		case "--":
			continue
		}
		if k == 1 {
			/* args[1] is the sandbox reference resolveAgent already consumed */
			continue
		}
		if !strings.HasPrefix(arg, "-") || arg == "-" {
			files = append(files, arg)
		}
	}
	if len(files) == 0 {
		fmt.Fprintf(os.Stderr, "error: no files to upload\n")
		return 1
	}

	tmpDir := sbx.TmpDir()
	dir := filepath.Join(tmpDir, "sbx-up-"+sbx.RandomHex(8))
	if err := sbx.Mkdirs(dir); err != nil {
		fmt.Fprintf(os.Stderr, "error: mkdir: %s\n", err)
		return 1
	}
	defer os.RemoveAll(dir)

	var names []string
	for _, f := range files {
		base := filepath.Base(f)
		if base == "" || base == "." || base == ".." {
			fmt.Fprintf(os.Stderr, "error: invalid file name '%s'\n", f)
			return 1
		}
		data, err := os.ReadFile(f)
		if err != nil {
			fmt.Fprintf(os.Stderr, "error: read %s: %s\n", f, err)
			return 1
		}
		if err := os.WriteFile(filepath.Join(dir, base), data, 0o644); err != nil {
			fmt.Fprintf(os.Stderr, "error: write: %s\n", err)
			return 1
		}
		names = append(names, base)
	}

	gzPath := filepath.Join(tmpDir, "sbx-upload-"+sbx.RandomHex(8)+".tar.gz")
	defer os.Remove(gzPath)
	if err := tarGz(gzPath, dir, names); err != nil {
		fmt.Fprintf(os.Stderr, "error: tar failed: %s\n", err)
		return 1
	}
	gzData, err := os.ReadFile(gzPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: read tar: %s\n", err)
		return 1
	}

	status, body := clientFor(ep).FilesWrite(dest, gzData)
	if status != 200 {
		fmt.Fprintf(os.Stderr, "upload failed: %d %s\n", status, body)
		return 1
	}
	fmt.Printf("uploaded %d file(s) to %s: %s\n", len(files), dest, body)
	return 0
}

// cmdDownload implements `sbx download [sandbox] <remote> [-o local]`.
func cmdDownload(args []string) int {
	ep := &agentEndpoint{port: 9119}
	if !resolveAgent(args, 1, ep) {
		return 1
	}

	var remote, local string
	for k := 1; k < len(args); k++ {
		arg := args[k]
		if arg == "-o" {
			if k+1 < len(args) {
				k++
				local = args[k]
			}
		} else if !strings.HasPrefix(arg, "-") || arg == "-" {
			if remote == "" {
				remote = arg
			} else {
				local = arg
			}
		}
	}
	if remote == "" {
		fmt.Fprintf(os.Stderr, "error: remote path required\n")
		return 1
	}
	if local == "" {
		local = filepath.Base(remote)
	}

	status, data := clientFor(ep).FilesRead(remote)
	if status == 404 {
		fmt.Fprintf(os.Stderr, "error: %s not found\n", remote)
		return 1
	}
	if status != 200 {
		fmt.Fprintf(os.Stderr, "download failed: %d\n", status)
		return 1
	}
	if err := os.WriteFile(local, data, 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "error: write %s: %s\n", local, err)
		return 1
	}
	fmt.Printf("downloaded %s -> %s (%d bytes)\n", remote, local, len(data))
	return 0
}

// tarGz packs the named files (relative to dir) into a gzip tar archive at out.
func tarGz(out, dir string, names []string) error {
	f, err := os.Create(out)
	if err != nil {
		return err
	}
	defer f.Close()
	gz := gzip.NewWriter(f)
	tw := tar.NewWriter(gz)
	for _, name := range names {
		path := filepath.Join(dir, name)
		info, err := os.Stat(path)
		if err != nil {
			return err
		}
		header, err := tar.FileInfoHeader(info, "")
		if err != nil {
			return err
		}
		header.Name = name
		if err := tw.WriteHeader(header); err != nil {
			return err
		}
		if !info.IsDir() {
			file, err := os.Open(path)
			if err != nil {
				return err
			}
			if _, err := io.Copy(tw, file); err != nil {
				file.Close()
				return err
			}
			file.Close()
		}
	}
	if err := tw.Close(); err != nil {
		return err
	}
	return gz.Close()
}
