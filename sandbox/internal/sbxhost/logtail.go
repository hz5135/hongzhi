package sbxhost

import "os"

// LogTail reads the last n lines of each existing file and joins them into a
// single debug string. Used by backends on failure to surface guest/vmm logs.
func LogTail(n int, paths ...string) string {
	out := ""
	for _, p := range paths {
		data, err := os.ReadFile(p)
		if err != nil || len(data) == 0 {
			continue
		}
		out += "\n--- " + p + " ---\n" + tailLines(data, n)
	}
	return out
}

func tailLines(data []byte, n int) string {
	lines := []string{}
	start := 0
	for i := 0; i < len(data); i++ {
		if data[i] == '\n' {
			lines = append(lines, string(data[start:i]))
			start = i + 1
		}
	}
	lines = append(lines, string(data[start:]))
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	out := ""
	for _, l := range lines {
		out += l + "\n"
	}
	return out
}
