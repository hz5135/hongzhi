// Package agentclient is a minimal HTTP client for the sbx-agent protocol
// (NDJSON event streaming for /exec, file upload/download, health probe).
package agentclient

import (
	"bufio"
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"time"
)

// Event is a single NDJSON event line emitted by POST /exec.
type Event struct {
	Type     string
	ID       string
	PID      int
	Data     string
	ExitCode int
	Error    string
}

// Client talks to a running sbx-agent over TCP.
type Client struct {
	Host  string
	Port  int
	Token string
	HTTP  *http.Client
}

func New(host string, port int, token string) *Client {
	// No client-level Timeout: POST /exec streams an NDJSON body for the whole
	// command lifetime, so an overall timeout would kill long-running execs.
	// Only the dial is bounded.
	transport := &http.Transport{DialContext: (&net.Dialer{Timeout: 5 * time.Second}).DialContext}
	return &Client{
		Host:  host,
		Port:  port,
		Token: token,
		HTTP:  &http.Client{Transport: transport},
	}
}

func (c *Client) baseURL() string {
	return fmt.Sprintf("http://%s:%d", c.Host, c.Port)
}

func (c *Client) headers() map[string]string {
	return map[string]string{"Authorization": "Bearer " + c.Token}
}

// Health returns the status code of GET /health (200 = healthy).
func (c *Client) Health() (int, string) {
	req, _ := http.NewRequest("GET", c.baseURL()+"/health", nil)
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return 0, err.Error()
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(body)
}

// Exec streams the NDJSON response of POST /exec, invoking onEvent for each
// parsed event. It returns the HTTP-level error status (0 = ok) after the
// stream ends. The callback returns false to abort the stream early.
func (c *Client) Exec(bodyJSON string, onEvent func(Event) bool) (int, error) {
	req, _ := http.NewRequest("POST", c.baseURL()+"/exec", bytes.NewReader([]byte(bodyJSON)))
	req.Header.Set("Content-Type", "application/json")
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		body, _ := io.ReadAll(resp.Body)
		return resp.StatusCode, fmt.Errorf("exec request failed: %s", string(body))
	}
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 64*1024), 64*1024*1024)
	for sc.Scan() {
		line := sc.Bytes()
		if len(bytes.TrimSpace(line)) == 0 {
			continue
		}
		var raw struct {
			Type     string `json:"type"`
			ID       string `json:"id"`
			PID      int    `json:"pid"`
			Data     string `json:"data"`
			ExitCode int    `json:"exitCode"`
			Error    string `json:"error"`
		}
		if err := json.Unmarshal(line, &raw); err != nil {
			continue
		}
		ev := Event{Type: raw.Type, ID: raw.ID, PID: raw.PID, Data: raw.Data,
			ExitCode: raw.ExitCode, Error: raw.Error}
		if !onEvent(ev) {
			break
		}
	}
	return 0, sc.Err()
}

// ExecStdin pushes raw bytes to a running tty command.
func (c *Client) ExecStdin(id, data string) {
	req, _ := http.NewRequest("POST", c.baseURL()+"/exec/"+id+"/stdin", bytes.NewReader([]byte(data)))
	req.Header.Set("Content-Type", "application/octet-stream")
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	resp, err := c.HTTP.Do(req)
	if err == nil {
		resp.Body.Close()
	}
}

// ExecResize resizes a running tty command.
func (c *Client) ExecResize(id string, rows, cols int) {
	body, _ := json.Marshal(map[string]int{"rows": rows, "cols": cols})
	req, _ := http.NewRequest("POST", c.baseURL()+"/exec/"+id+"/resize", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	resp, err := c.HTTP.Do(req)
	if err == nil {
		resp.Body.Close()
	}
}

// FilesWrite uploads a gzip tar payload, extracted under extractDir.
func (c *Client) FilesWrite(extractDir string, gzData []byte) (int, string) {
	req, _ := http.NewRequest("POST", c.baseURL()+"/files/write", bytes.NewReader(gzData))
	req.Header.Set("Content-Type", "application/gzip")
	req.Header.Set("X-Extract-Dir", extractDir)
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return 0, err.Error()
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(body)
}

// FilesRead downloads a file from the guest as raw bytes.
func (c *Client) FilesRead(path string) (int, []byte) {
	body, _ := json.Marshal(map[string]string{"path": path})
	req, _ := http.NewRequest("POST", c.baseURL()+"/files/read", bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	for k, v := range c.headers() {
		req.Header.Set(k, v)
	}
	resp, err := c.HTTP.Do(req)
	if err != nil {
		return 0, nil
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, data
}
