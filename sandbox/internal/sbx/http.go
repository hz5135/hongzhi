package sbx

import (
	"io"
	"net/http"
	"strings"
	"time"
)

// agentTransport carries host -> guest agent requests. Its Proxy is explicitly
// nil, which is the reason this type exists rather than the default transport.
//
// The agent is reached over the sandbox's own TAP at the guest's 198.19.x.y,
// and the default transport routes that through the *caller's* HTTP_PROXY /
// http_proxy - an environment the sandbox knows nothing about. A proxied probe
// cannot reach the guest at all, so a perfectly healthy sandbox is declared
// dead: the request goes to the proxy, the proxy waits out its own connection
// attempt to a private address, and this client's timeout cuts the wait off
// first. That is what `did not become healthy ... Client.Timeout exceeded while
// awaiting headers` meant on a host whose shell exported http_proxy, with the
// VM alive, the TAP up, the guest's eth0 configured and its agent already
// listening. Go bypasses the proxy for loopback on its own, so the container
// backends, which probe 127.0.0.1, never showed this.
//
// The exec/file clients (agentclient) already build their own transport and are
// proxy-free for the same reason; keep them that way.
var agentTransport = &http.Transport{Proxy: nil}

// agentHTTPGet performs a plain TCP HTTP GET against the agent, returning the
// status code and body. Used by waitAgent for the /health probe.
func agentHTTPGet(host string, port int, path string, headers map[string]string) Response {
	client := &http.Client{Timeout: 5 * time.Second, Transport: agentTransport}
	url := "http://" + host + ":" + itoa(port) + path
	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		return Response{Status: 0, Err: err}
	}
	for k, v := range headers {
		req.Header.Set(k, v)
	}
	resp, err := client.Do(req)
	if err != nil {
		return Response{Status: 0, Err: err}
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return Response{Status: resp.StatusCode, Body: strings.TrimSpace(string(body))}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	neg := n < 0
	if neg {
		n = -n
	}
	var b [20]byte
	i := len(b)
	for n > 0 {
		i--
		b[i] = byte('0' + n%10)
		n /= 10
	}
	if neg {
		i--
		b[i] = '-'
	}
	return string(b[i:])
}
