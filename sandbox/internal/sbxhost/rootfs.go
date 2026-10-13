package sbxhost

import (
	"archive/tar"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/docker/docker/api/types"
	"github.com/docker/docker/api/types/container"
	"github.com/docker/docker/client"

	"agentbench/sandbox/internal/sbx"
)

// The derived image only adds what a microVM guest needs on top of the source
// image: systemd as init, and the tools the agent uses. It deliberately does not
// rewrite the apt sources - the image's own mirror is the one its build worked
// against, and a hard-coded mirror that happens to be unreachable on the user's
// network turns this rebuild (which every image change triggers) into a hang.
// apt's own timeouts are bounded so a broken mirror fails the create with a
// message instead of stalling it past the caller's timeout.
const rootfsDockerfile = "FROM %s\n" +
	"RUN printf 'Acquire::http::Timeout \"15\";\\nAcquire::https::Timeout \"15\";\\nAcquire::Retries \"2\";\\n' " +
	"> /etc/apt/apt.conf.d/99-agentbench-timeouts\n" +
	"RUN apt-get update && apt-get install -y --no-install-recommends systemd systemd-sysv " +
	"iproute2 iptables iputils-ping procps sudo tar gzip ca-certificates && rm -rf /var/lib/apt/lists/*\n"

// GuestBootInitPath is where InjectAgentFiles installs the boot-timer wrapper,
// and what the firecracker backend boots the guest with (`init=`). firecracker's
// `--boot-timer` only emits its `Guest-boot-time` record when a guest writes the
// magic byte to its boot device, and this wrapper is the guest doing it - from
// the same place upstream's reference guest does, right before the real init, so
// the record ends where the guest's own init begins and no userspace boot time
// is counted.
const GuestBootInitPath = "/usr/local/bin/sbx-boot-init"

// agentUnit is the systemd unit that launches the in-guest agent for microVMs.
// The per-VM token is passed on the kernel command line (agentbench.token=...)
// and read by the agent with --token-from-cmdline, so the injected rootfs is a
// fully static, reusable template.
const agentUnit = `[Unit]
Description=SBX Agent
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/sbx-agent --token-from-cmdline
Restart=always
RestartSec=2
# Keep the agent's own output on the guest disk: it is the only record of
# whether the agent ever started, and firecracker's Sync io engine commits it
# to the host-side image, where a failed create can read it back.
StandardOutput=append:/var/log/sbx-agent.log
StandardError=append:/var/log/sbx-agent.log

[Install]
WantedBy=multi-user.target
`

// bootLogUnit dumps what the guest knows about its own boot into the guest disk
// on every boot. A sandbox that never brings its agent up leaves nothing else
// behind: the guest console goes to firecracker's stdout, which the jailer sends
// to /dev/null, so without this a failed create cannot say whether the kernel
// came up, whether the network was configured, or how far init got. It is
// ordered after the agent rather than before it: a diagnostic has no business
// on the startup path, and `Before=sbx-agent.service` put its whole dmesg/sync
// run between local-fs.target and the agent (56 ms of the unit, and 41 ms of
// the `ready` total it sits inside).
const bootLogUnit = `[Unit]
Description=SBX boot log
DefaultDependencies=no
After=local-fs.target
After=sbx-agent.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/sh -c 'touch /var/log/sbx-agent.log; exec >/var/log/sbx-boot.log 2>&1; date -Is; cat /proc/uptime; cat /proc/cmdline; ip -br addr; echo --- dmesg; dmesg; echo --- failed units; systemctl --no-pager --failed; sync'

[Install]
WantedBy=multi-user.target
`

// registryRootfs resolves the cache dir for an image:tag under
// ~/.sbx/registry/rootfs/<name>/<tag>.
func registryRootfs(image string) (name, tag, cache string) {
	name, tag = image, "latest"
	if idx := strings.Index(image, ":"); idx >= 0 {
		name, tag = image[:idx], image[idx+1:]
	}
	cache = filepath.Join(sbx.HomeDir(), "registry", "rootfs", name, tag)
	return
}

func newDockerClient() (*client.Client, error) {
	return client.NewClientWithOpts(client.FromEnv, client.WithAPIVersionNegotiation())
}

func dockerImageID(ctx context.Context, cli *client.Client, image string) string {
	img, _, err := cli.ImageInspectWithRaw(ctx, image)
	if err != nil {
		return ""
	}
	return img.ID
}

// buildContextTar packs a Dockerfile into an in-memory tar build context.
func buildContextTar(dockerfile string) (io.Reader, error) {
	var buf bytes.Buffer
	tw := tar.NewWriter(&buf)
	content := []byte(dockerfile)
	if err := tw.WriteHeader(&tar.Header{Name: "Dockerfile", Mode: 0o644, Size: int64(len(content))}); err != nil {
		return nil, err
	}
	if _, err := tw.Write(content); err != nil {
		return nil, err
	}
	if err := tw.Close(); err != nil {
		return nil, err
	}
	return &buf, nil
}

// buildImage builds tagName from the rootfs Dockerfile and drains the build
// stream, decoding errors from the daemon's JSON build output.
func buildImage(ctx context.Context, cli *client.Client, image, tagName string) error {
	reader, err := buildContextTar(fmt.Sprintf(rootfsDockerfile, image))
	if err != nil {
		return err
	}
	resp, err := cli.ImageBuild(ctx, reader, types.ImageBuildOptions{
		Dockerfile: "Dockerfile",
		Tags:       []string{tagName},
		Remove:     true,
	})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	var lastErr string
	dec := json.NewDecoder(resp.Body)
	for {
		var msg struct {
			Error string `json:"error"`
		}
		if derr := dec.Decode(&msg); derr != nil {
			break
		}
		if msg.Error != "" {
			lastErr = msg.Error
		}
	}
	if lastErr != "" {
		return fmt.Errorf("docker build: %s", lastErr)
	}
	return nil
}

// RootfsState describes the cached guest rootfs of one image: the pristine base
// (rootfs.ext4 - the Docker image exported into an ext4 filesystem with init and
// the guest tools installed) and the agent-injected template (injected.ext4)
// that the microVM backends boot.
type RootfsState struct {
	Image          string
	ImageID        string // id of the Docker image; "" when it could not be resolved
	CacheDir       string
	Base           string
	BaseID         string // image id the base was built from; "" = never built
	BaseSize       int64
	Injected       string
	InjectedID     string // image id the injection was made from
	InjectedHash   string // hash of the sbx-agent baked into injected.ext4
	InjectedLayout string // version of the file set baked into injected.ext4
	AgentHash      string // hash of the sbx-agent the caller would inject
	InjectedSize   int64
}

// InjectedLayoutVersion is the version of the file set InjectAgentFiles
// installs. Bump it whenever that set changes: .agenthash alone cannot tell a
// template built before a new injected file existed from a current one, and the
// firecracker backend boots the guest with an `init=` path the template has to
// contain.
const InjectedLayoutVersion = "2"

// InjectedLayoutCurrent reports whether the template carries the file set the
// current InjectAgentFiles installs.
func (s RootfsState) InjectedLayoutCurrent() bool {
	return s.InjectedLayout == InjectedLayoutVersion
}

// BaseCurrent reports whether the base template exists and was built from the
// image the daemon currently holds under that tag. An image id that cannot be
// resolved (no Docker daemon, or the image is gone) leaves an existing template
// valid: it is a complete artifact, and running microVMs needs no Docker at all.
func (s RootfsState) BaseCurrent() bool {
	if !sbx.FileExists(s.Base) {
		return false
	}
	return s.ImageID == "" || s.BaseID == s.ImageID
}

// InjectedCurrent reports whether the injected template matches the base it was
// made from, the layout this build installs and the agent binary the caller
// would inject.
func (s RootfsState) InjectedCurrent() bool {
	if !sbx.FileExists(s.Injected) || !s.BaseCurrent() {
		return false
	}
	if s.InjectedID != s.BaseID || !s.InjectedLayoutCurrent() {
		return false
	}
	return s.AgentHash == "" || s.InjectedHash == s.AgentHash
}

// RootfsBuild is what PrepareRootfs did (and how long it took).
type RootfsBuild struct {
	State           RootfsState
	BaseRebuilt     bool
	InjectedRebuilt bool
	BaseSec         float64
	InjectSec       float64
}

// ShortID renders a Docker image id for a message (they are 71 characters long,
// which buries the part that differs).
func ShortID(id string) string {
	if len(id) > 19 {
		return id[:19]
	}
	if id == "" {
		return "unknown"
	}
	return id
}

func inspectRootfs(image, agentBin, imageID string) RootfsState {
	_, _, cache := registryRootfs(image)
	st := RootfsState{
		Image:    image,
		ImageID:  imageID,
		CacheDir: cache,
		Base:     filepath.Join(cache, "rootfs.ext4"),
		Injected: filepath.Join(cache, "injected.ext4"),
	}
	st.BaseID = strings.TrimSpace(sbx.ReadFile(st.Base + ".imageid"))
	st.InjectedID = strings.TrimSpace(sbx.ReadFile(st.Injected + ".baseid"))
	st.InjectedHash = strings.TrimSpace(sbx.ReadFile(st.Injected + ".agenthash"))
	st.InjectedLayout = strings.TrimSpace(sbx.ReadFile(st.Injected + ".layout"))
	if fi, err := os.Stat(st.Base); err == nil {
		st.BaseSize = fi.Size()
	}
	if fi, err := os.Stat(st.Injected); err == nil {
		st.InjectedSize = fi.Size()
	}
	if agentBin != "" {
		st.AgentHash = sha256File(agentBin)
	}
	return st
}

// dockerImageIDFor resolves the id an image tag points at now, "" when the
// daemon is unreachable or does not have it.
func dockerImageIDFor(image string) string {
	cli, err := newDockerClient()
	if err != nil {
		return ""
	}
	defer cli.Close()
	return dockerImageID(context.Background(), cli, image)
}

// RootfsStatus reports the cached guest rootfs of an image without changing
// anything. Used by `sbx rootfs status` and `sbx doctor`.
func RootfsStatus(image, agentBin string) RootfsState {
	return inspectRootfs(image, agentBin, dockerImageIDFor(image))
}

// lockCache takes the exclusive lock that serializes template changes for one
// image (concurrent creates share one cache). Released by closing the file.
func lockCache(cache string) (*os.File, error) {
	lf, err := os.OpenFile(filepath.Join(cache, ".ensure.lock"), os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(lf.Fd()), syscall.LOCK_EX); err != nil {
		lf.Close()
		return nil, err
	}
	return lf, nil
}

func unlockCache(lf *os.File) {
	syscall.Flock(int(lf.Fd()), syscall.LOCK_UN)
	lf.Close()
}

// PrepareRootfs makes the cached guest rootfs of an image current: it builds the
// base (docker build -> docker export -> mkfs.ext4) when the image id no longer
// matches the cache, and bakes the agent into injected.ext4 when the base or the
// agent binary changed.
//
// This is the only place that builds a rootfs, and it is driven by the build
// (`make rootfs` -> `sbx rootfs build`), never by a benchmark run: creating a
// sandbox only copies the finished template. A build here costs tens of seconds
// (docker build + export + mkfs of the whole image), so doing it on the first
// create after every image rebuild puts one-time work inside a measurement -
// and under `-n N` every parallel create wakes a build of its own.
func PrepareRootfs(image, agentBin string, force bool) (*RootfsBuild, error) {
	_, _, cache := registryRootfs(image)
	if err := sbx.Mkdirs(cache); err != nil {
		return nil, err
	}
	if !sbx.FileExists(agentBin) {
		return nil, fmt.Errorf("sandbox agent binary not found at %s - build it with "+
			"`make agent` (or point SBX_AGENT_BIN at it)", agentBin)
	}
	lf, lerr := lockCache(cache)
	if lerr != nil {
		return nil, lerr
	}
	defer unlockCache(lf)

	cli, cerr := newDockerClient()
	if cerr != nil {
		return nil, cerr
	}
	defer cli.Close()
	ctx := context.Background()
	imageID := dockerImageID(ctx, cli, image)
	if imageID == "" {
		return nil, fmt.Errorf("docker image %s is not in the local daemon - build it "+
			"first (make image, or ./scripts/build-image.sh)", image)
	}

	out := &RootfsBuild{}
	st := inspectRootfs(image, agentBin, imageID)
	if force || !st.BaseCurrent() {
		t0 := time.Now()
		if berr := buildBaseRootfs(ctx, cli, image, st); berr != nil {
			return nil, berr
		}
		out.BaseSec = time.Since(t0).Seconds()
		out.BaseRebuilt = true
		st = inspectRootfs(image, agentBin, imageID)
	}
	if force || !st.InjectedCurrent() {
		t0 := time.Now()
		if ierr := injectInto(st.Base, agentBin, st.Injected, cache); ierr != nil {
			return nil, ierr
		}
		sbx.WriteFile(st.Injected+".agenthash", st.AgentHash)
		sbx.WriteFile(st.Injected+".baseid", st.BaseID)
		sbx.WriteFile(st.Injected+".layout", InjectedLayoutVersion)
		/* A template build writes gigabytes (the docker export, its extraction and
		 * both images) and deletes some of it again. Left dirty, that writeback
		 * lands on the guest's first boot - firecracker serves its block I/O from
		 * the same cache and the host storage is busy - and a guest that stalls
		 * there never brings its agent up, which shows as a create-time health
		 * failure right after every image rebuild. Flush before the VM starts. */
		syscall.Sync()
		out.InjectSec = time.Since(t0).Seconds()
		out.InjectedRebuilt = true
		st = inspectRootfs(image, agentBin, imageID)
	}
	out.State = st
	return out, nil
}

// buildBaseRootfs builds the pristine base template: the derived image (init and
// the guest tools on top of the source image), exported and written into a fresh
// ext4 filesystem. The .imageid marker is written last, so an interrupted build
// is never mistaken for a current template.
func buildBaseRootfs(ctx context.Context, cli *client.Client, image string, st RootfsState) error {
	name, tag, cache := registryRootfs(image)
	tagName := "sbx-rootfs-" + name + ":" + tag
	if err := buildImage(ctx, cli, image, tagName); err != nil {
		return err
	}

	ext4 := st.Base
	cname := "sbx-export-" + sbx.RandomHex(6)
	tmpTar := filepath.Join(cache, "rootfs.tar")
	tmpExtract := filepath.Join(cache, "rootfs-extracted")
	defer func() {
		cli.ContainerRemove(ctx, cname, container.RemoveOptions{Force: true})
		os.Remove(tmpTar)
		os.RemoveAll(tmpExtract)
	}()

	created, err := cli.ContainerCreate(ctx, &container.Config{Image: tagName}, nil, nil, nil, cname)
	if err != nil {
		return fmt.Errorf("docker create: %s", err)
	}
	export, err := cli.ContainerExport(ctx, created.ID)
	if err != nil {
		return fmt.Errorf("docker export: %s", err)
	}
	tf, err := os.Create(tmpTar)
	if err != nil {
		export.Close()
		return err
	}
	if _, err := io.Copy(tf, export); err != nil {
		tf.Close()
		export.Close()
		return err
	}
	tf.Close()
	export.Close()

	os.RemoveAll(tmpExtract)
	if err := os.MkdirAll(tmpExtract, 0o755); err != nil {
		return err
	}
	if out, err := exec.Command("tar", "-xf", tmpTar, "-C", tmpExtract).CombinedOutput(); err != nil {
		return fmt.Errorf("tar extract: %s%s", err, out)
	}

	tst, err := os.Stat(tmpTar)
	if err != nil {
		return err
	}
	mb := tst.Size()/(1024*1024) + 512
	if mb < 1024 {
		mb = 1024
	}
	// Start from a fresh file: mkfs.ext4 would otherwise ask before replacing an
	// existing filesystem, and the marker below is written only on success.
	os.Remove(ext4)
	if out, err := exec.Command("mkfs.ext4", "-q", "-d", tmpExtract, ext4,
		fmt.Sprintf("%dM", mb)).CombinedOutput(); err != nil {
		return fmt.Errorf("mkfs.ext4: %s%s", err, out)
	}
	exec.Command("tune2fs", "-m", "0", ext4).Run()
	return sbx.WriteFile(ext4+".imageid", st.ImageID)
}

// EnsureInjectedRootfs returns the agent-injected rootfs template to boot for an
// image.
//
// The template is prepared by the build (`make rootfs` -> `sbx rootfs build`); it
// is not built here. When it is missing or stale this says what to run and stops,
// because building one is tens of seconds of one-time work that has no business
// running inside a benchmark, and because every parallel create would otherwise
// start a build of its own. Injecting the agent into an up-to-date base is a
// local sub-second copy, so it still happens here (it is what a rebuilt
// `sbx-agent` changes).
func EnsureInjectedRootfs(image, agentBin string) (string, error) {
	_, _, cache := registryRootfs(image)
	if err := sbx.Mkdirs(cache); err != nil {
		return "", err
	}
	st := inspectRootfs(image, agentBin, dockerImageIDFor(image))
	if !st.BaseCurrent() {
		return "", rootfsNotReadyError(st)
	}
	if st.InjectedCurrent() {
		return st.Injected, nil
	}

	lf, lerr := lockCache(cache)
	if lerr != nil {
		return "", lerr
	}
	defer unlockCache(lf)
	// Another create may have injected it while we waited for the lock.
	st = inspectRootfs(image, agentBin, st.ImageID)
	if st.InjectedCurrent() {
		return st.Injected, nil
	}
	if ierr := injectInto(st.Base, agentBin, st.Injected, cache); ierr != nil {
		return "", ierr
	}
	sbx.WriteFile(st.Injected+".agenthash", st.AgentHash)
	sbx.WriteFile(st.Injected+".baseid", st.BaseID)
	sbx.WriteFile(st.Injected+".layout", InjectedLayoutVersion)
	syscall.Sync()
	return st.Injected, nil
}

// rootfsNotReadyError says that the guest rootfs template has to be built, and
// how. It carries sbx.ErrRootfsNotPrepared so the CLI can exit with a code that
// tells a driver "the environment is not prepared" - fix it, do not retry.
func rootfsNotReadyError(st RootfsState) error {
	fix := fmt.Sprintf("prepared it with `make rootfs` (or `sbx rootfs build --image %s`)",
		st.Image)
	if st.BaseID == "" {
		return fmt.Errorf("%w: no guest rootfs template for %s (%s); %s",
			sbx.ErrRootfsNotPrepared, st.Image, st.Base, fix)
	}
	return fmt.Errorf("%w: the guest rootfs template for %s was built from image %s, but "+
		"%s is now %s; %s", sbx.ErrRootfsNotPrepared, st.Image, ShortID(st.BaseID),
		st.Image, ShortID(st.ImageID), fix)
}

// injectInto builds the injected template by copying the pristine rootfs to a
// temp file and mounting only that copy, so the pristine rootfs.ext4 is never
// written to. The result is atomically renamed into place.
func injectInto(ext4, agentBin, outExt4, cache string) error {
	mnt := filepath.Join(cache, "inject-mnt")
	tmp := outExt4 + ".tmp." + sbx.RandomHex(6)
	os.RemoveAll(mnt)
	if err := os.MkdirAll(mnt, 0o755); err != nil {
		return err
	}
	defer os.RemoveAll(mnt)
	// Same reasoning as CopyFile: the injection copies the whole template, and a
	// reflink makes it a metadata operation when the cache lives on XFS/btrfs.
	// The mounted, written copy is a reflink too - CoW keeps the template intact.
	cpArgs := []string{"--sparse=always", "--reflink=auto", ext4, tmp}
	if out, err := exec.Command("cp", cpArgs...).CombinedOutput(); err != nil {
		os.Remove(tmp)
		return fmt.Errorf("cp rootfs for injection: %s%s", err, out)
	}
	defer os.Remove(tmp)
	if out, err := exec.Command("mount", "-o", "loop", tmp, mnt).CombinedOutput(); err != nil {
		return fmt.Errorf("mount for injection: %s%s", err, out)
	}
	defer exec.Command("umount", mnt).Run()
	if ierr := InjectAgentFiles(mnt, agentBin); ierr != nil {
		return ierr
	}
	exec.Command("sync").Run()
	if err := os.Rename(tmp, outExt4); err != nil {
		return fmt.Errorf("rename injected: %s", err)
	}
	return nil
}

// InjectAgentFiles writes the static in-guest agent content into a mounted
// rootfs: the binary (installed twice - see GuestBootInitPath), systemd unit,
// resolv.conf and hostname.
func InjectAgentFiles(mnt, agentBin string) error {
	agentDst := filepath.Join(mnt, "usr/local/bin/sbx-agent")
	// The boot-timer wrapper is the same static binary under a second name, and
	// picks that mode from argv[0]: one artifact, so .agenthash covers both
	// files and the template stays the fully static one every VM reuses.
	bootInitDst := filepath.Join(mnt, GuestBootInitPath)
	unitDir := filepath.Join(mnt, "etc/systemd/system")
	wants := filepath.Join(unitDir, "multi-user.target.wants")
	for _, d := range []string{filepath.Join(mnt, "usr/local/bin"), unitDir, wants} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			return err
		}
	}
	if err := CopyFile(agentBin, agentDst); err != nil {
		return err
	}
	os.Chmod(agentDst, 0o755)
	if err := CopyFile(agentBin, bootInitDst); err != nil {
		return err
	}
	os.Chmod(bootInitDst, 0o755)
	if err := os.WriteFile(filepath.Join(unitDir, "sbx-agent.service"), []byte(agentUnit), 0o644); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(unitDir, "sbx-bootlog.service"), []byte(bootLogUnit), 0o644); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Join(mnt, "var/log"), 0o755); err != nil {
		return err
	}
	// Enable the unit: systemd only starts services that are wanted by a target.
	if err := os.Symlink("/etc/systemd/system/sbx-bootlog.service", filepath.Join(wants, "sbx-bootlog.service")); err != nil {
		return err
	}
	if err := os.Symlink("/etc/systemd/system/sbx-agent.service", filepath.Join(wants, "sbx-agent.service")); err != nil {
		return err
	}
	resolv := filepath.Join(mnt, "etc/resolv.conf")
	os.Remove(resolv)
	if err := os.Symlink("/proc/net/pnp", resolv); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(mnt, "etc/hostname"), []byte("agentbench\n"), 0o644)
}

func sha256File(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	h := sha256.New()
	io.Copy(h, f)
	return hex.EncodeToString(h.Sum(nil))
}

// Ext4Tail reads the tail of files inside an ext4 image with debugfs - no mount
// and no write, so it is safe on an image a running VM has open. Used to get
// the guest's own boot/agent log out of a sandbox that failed to come up.
func Ext4Tail(image string, lines int, guestPaths ...string) string {
	if !sbx.FileExists(image) {
		return ""
	}
	out := ""
	for _, p := range guestPaths {
		cmd := exec.Command("debugfs", "-R", "cat "+p, image)
		data, err := cmd.Output()
		if err != nil {
			out += "\n--- " + p + " (in the guest rootfs) ---\n(no such file: the guest never\n" +
				"reached the unit that writes it)\n"
			continue
		}
		if len(data) == 0 {
			out += "\n--- " + p + " (in the guest rootfs) ---\n(file is empty)\n"
			continue
		}
		out += "\n--- " + p + " (in the guest rootfs) ---\n" + tailLines(data, lines)
	}
	return out
}
