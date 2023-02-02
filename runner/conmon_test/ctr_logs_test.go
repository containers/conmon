package conmon_test

import (
	"io/ioutil"
	"os"
	"path/filepath"

	"github.com/containers/conmon/runner/conmon"
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

var _ = Describe("conmon ctr logs", func() {
	var tmpDir string
	var tmpLogPath string
	const invalidLogDriver = "invalid"
	BeforeEach(func() {
		d, err := ioutil.TempDir(os.TempDir(), "conmon-")
		Expect(err).To(BeNil())
		tmpDir = d
		tmpLogPath = filepath.Join(tmpDir, "log")
	})
	AfterEach(func() {
		Expect(os.RemoveAll(tmpDir)).To(BeNil())
	})
	It("no log driver should fail", func() {
		_, stderr := getConmonOutputGivenLogOpts()
		Expect(stderr).To(ContainSubstring("Log driver not provided. Use --log-path"))
	})
	It("log driver as path should pass", func() {
		_, stderr := getConmonOutputGivenLogOpts(conmon.WithLogDriver("", tmpLogPath))
		Expect(stderr).To(BeEmpty())

		_, err := os.Stat(tmpLogPath)
		Expect(err).To(BeNil())
	})
	It("log driver as journald should pass", func() {
		_, stderr := getConmonOutputGivenLogOpts(conmon.WithLogDriver("journald", ""))
		Expect(stderr).To(BeEmpty())
	})
	It("log driver as journald with short cid should fail", func() {
		// conmon requires a cid of len > 12
		shortCtrID := "abcdefghijkl"

		_, stderr := getConmonOutputGivenLogOpts(
			conmon.WithLogDriver("journald", ""),
			conmon.WithContainerID(shortCtrID),
		)
		Expect(stderr).To(ContainSubstring("Container ID must be longer than 12 characters"))
	})
	It("log driver as k8s-file with path should pass", func() {
		_, stderr := getConmonOutputGivenLogOpts(conmon.WithLogDriver("k8s-file", tmpLogPath))
		Expect(stderr).To(BeEmpty())

		_, err := os.Stat(tmpLogPath)
		Expect(err).To(BeNil())
	})
	It("log driver as passthrough should pass", func() {
		stdout, stderr := getConmonOutputGivenLogOpts(conmon.WithLogDriver("passthrough", ""))
		Expect(stdout).To(BeEmpty())
		Expect(stderr).To(BeEmpty())
	})
	It("log driver as k8s-file with invalid path should fail", func() {
		_, stderr := getConmonOutputGivenLogOpts(conmon.WithLogDriver("k8s-file", invalidPath))
		Expect(stderr).To(ContainSubstring("Failed to open log file"))
	})
	It("log driver as invalid driver should fail", func() {
		_, stderr := getConmonOutputGivenLogOpts(conmon.WithLogDriver(invalidLogDriver, tmpLogPath))
		Expect(stderr).To(ContainSubstring("No such log driver " + invalidLogDriver))
	})
	It("multiple log drivers should pass", func() {
		_, stderr := getConmonOutputGivenLogOpts(
			conmon.WithLogDriver("k8s-file", tmpLogPath),
			conmon.WithLogDriver("journald", ""),
		)
		Expect(stderr).To(BeEmpty())

		_, err := os.Stat(tmpLogPath)
		Expect(err).To(BeNil())
	})
	It("multiple log drivers with one invalid should fail", func() {
		_, stderr := getConmonOutputGivenLogOpts(
			conmon.WithLogDriver("k8s-file", tmpLogPath),
			conmon.WithLogDriver(invalidLogDriver, tmpLogPath),
		)
		Expect(stderr).To(ContainSubstring("No such log driver " + invalidLogDriver))
	})
})

func getConmonOutputGivenLogOpts(logDriverOpts ...conmon.ConmonOption) (string, string) {
	opts := []conmon.ConmonOption{
		conmon.WithPath(conmonPath),
		conmon.WithContainerID(ctrID),
		conmon.WithContainerUUID(ctrID),
		conmon.WithRuntimePath(validPath),
	}
	opts = append(opts, logDriverOpts...)
	return getConmonOutputGivenOptions(opts...)
}
