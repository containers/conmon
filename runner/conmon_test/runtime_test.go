package conmon_test

import (
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"

	"github.com/containers/conmon/runner/conmon"
	"github.com/containers/storage/pkg/stringid"
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	"github.com/opencontainers/runtime-tools/generate"
)

var _ = Describe("runc", func() {
	var (
		tmpDir     string
		tmpLogPath string
		tmpPidFile string
		tmpRootfs  string
	)
	BeforeEach(func() {
		// save busy box binary if we don't have it
		Expect(cacheBusyBox()).To(BeNil())

		// create tmpDir
		d, err := ioutil.TempDir(os.TempDir(), "conmon-")
		Expect(err).To(BeNil())
		tmpDir = d

		// generate logging path
		tmpLogPath = filepath.Join(tmpDir, "log")

		// generate container ID
		ctrID = stringid.GenerateNonCryptoID()

		// create the rootfs of the "container"
		tmpRootfs = filepath.Join(tmpDir, "rootfs")
		Expect(os.MkdirAll(tmpRootfs, 0755)).To(BeNil())

		tmpPidFile = filepath.Join(tmpDir, "pidfile")

		Expect(os.Link(busyboxDest, filepath.Join(tmpRootfs, "busybox"))).To(BeNil())

		// finally, create config.json
		_, err = generateRuntimeConfig(tmpDir, tmpRootfs)
		Expect(err).To(BeNil())
	})
	AfterEach(func() {
		Expect(os.RemoveAll(tmpDir)).To(BeNil())
		Expect(runRuntimeCommand("delete", "-f", ctrID)).To(BeNil())
	})
	It("simple runtime test", func() {
		stdout, stderr := getConmonOutputGivenOptions(
			conmon.WithPath(conmonPath),
			conmon.WithContainerID(ctrID),
			conmon.WithContainerUUID(ctrID),
			conmon.WithRuntimePath(runtimePath),
			conmon.WithLogDriver("k8s-file", tmpLogPath),
			conmon.WithBundlePath(tmpDir),
			conmon.WithSocketPath(tmpDir),
			conmon.WithSyslog(),
			conmon.WithLogLevel("trace"),
			conmon.WithContainerPidFile(tmpPidFile),
			conmon.WithConmonPidFile(fmt.Sprintf("%s/conmon-pidfile", tmpDir)),
			conmon.WithSyncPipe(),
		)
		Expect(stdout).To(BeEmpty())
		Expect(stderr).To(BeEmpty())

		Expect(runRuntimeCommand("start", ctrID)).To(BeNil())

		Expect(getFileContents(tmpLogPath)).To(ContainSubstring("busybox"))
		Expect(getFileContents(tmpPidFile)).To(Not(BeEmpty()))
	})
})

func getFileContents(filename string) string {
	b, err := ioutil.ReadFile(filename)
	Expect(err).To(BeNil())
	return string(b)
}

func generateRuntimeConfig(bundlePath, rootfs string) (string, error) {
	configPath := filepath.Join(bundlePath, "config.json")
	g, err := generate.New("linux")
	if err != nil {
		return "", err
	}
	g.SetProcessCwd("/")
	g.SetProcessArgs([]string{"/busybox", "ls"})
	g.SetRootPath(rootfs)

	if err := g.SaveToFile(configPath, generate.ExportOptions{}); err != nil {
		return "", err
	}
	return configPath, nil
}
