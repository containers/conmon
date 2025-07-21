package conmon_test

import (
	"os"
	"path/filepath"

	"github.com/containers/conmon/runner/conmon"
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

var _ = Describe("k8s-file log rotation", func() {
	var tmpDir string
	var tmpLogPath string

	BeforeEach(func() {
		tmpDir = GinkgoT().TempDir()
		tmpLogPath = filepath.Join(tmpDir, "container.log")
	})

	// Test that k8s-file log driver creates properly formatted logs
	It("should create valid k8s log format", func() {
		stdout, stderr := getConmonOutputGivenOptions(
			conmon.WithPath(conmonPath),
			conmon.WithContainerID(ctrID),
			conmon.WithContainerUUID(ctrID),
			conmon.WithRuntimePath(validPath),
			conmon.WithLogDriver("k8s-file", tmpLogPath),
		)

		Expect(stdout).To(BeEmpty())
		Expect(stderr).To(BeEmpty())

		// Verify that log file exists
		_, err := os.Stat(tmpLogPath)
		Expect(err).To(BeNil(), "Log file should be created")
	})

	// Test that log size max option is accepted without errors
	It("should accept log-size-max option", func() {
		logSizeMax := int64(1024)

		stdout, stderr := getConmonOutputGivenOptions(
			conmon.WithPath(conmonPath),
			conmon.WithContainerID(ctrID),
			conmon.WithContainerUUID(ctrID),
			conmon.WithRuntimePath(validPath),
			conmon.WithLogDriver("k8s-file", tmpLogPath),
			conmon.WithLogSizeMax(logSizeMax),
		)

		Expect(stdout).To(BeEmpty())
		Expect(stderr).To(BeEmpty())

		// Verify that log file exists
		_, err := os.Stat(tmpLogPath)
		Expect(err).To(BeNil(), "Log file should be created")
	})

	// Test that multiple log drivers work with size limits
	It("should handle multiple log drivers with size limits", func() {
		logSizeMax := int64(2048)

		stdout, stderr := getConmonOutputGivenOptions(
			conmon.WithPath(conmonPath),
			conmon.WithContainerID(ctrID),
			conmon.WithContainerUUID(ctrID),
			conmon.WithRuntimePath(validPath),
			conmon.WithLogDriver("k8s-file", tmpLogPath),
			conmon.WithLogDriver("journald", ""),
			conmon.WithLogSizeMax(logSizeMax),
		)

		Expect(stdout).To(BeEmpty())
		Expect(stderr).To(BeEmpty())

		// Verify that log file exists
		_, err := os.Stat(tmpLogPath)
		Expect(err).To(BeNil(), "Log file should be created")
	})
})
