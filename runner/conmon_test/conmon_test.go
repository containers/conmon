package conmon_test

import (
	"fmt"
	"io/ioutil"
	"os"
	"path/filepath"

	"github.com/containers/conmon/runner/conmon"
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
	"github.com/pkg/errors"
	"golang.org/x/sys/unix"
)

var _ = Describe("conmon", func() {
	Describe("version", func() {
		It("Should return conmon version", func() {
			out, _ := getConmonOutputGivenOptions(
				conmon.WithVersion(),
				conmon.WithPath(conmonPath),
			)
			Expect(out).To(ContainSubstring("conmon version"))
			Expect(out).To(ContainSubstring("commit"))
		})
	})
	Describe("no container ID", func() {
		It("should fail", func() {
			_, err := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
			)
			Expect(err).To(ContainSubstring("conmon: Container ID not provided. Use --cid"))
		})
	})
	Describe("no container UUID", func() {
		It("should fail", func() {
			_, err := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
			)
			Expect(err).To(ContainSubstring("Container UUID not provided. Use --cuuid"))
		})
	})
	Describe("runtime path", func() {
		It("no path should fail", func() {
			_, err := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
			)
			Expect(err).To(ContainSubstring("Runtime path not provided. Use --runtime"))
		})
		It("invalid path should fail", func() {
			_, err := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(invalidPath),
			)
			Expect(err).To(ContainSubstring(fmt.Sprintf("Runtime path %s is not valid", invalidPath)))
		})
	})
	Describe("ctr logs", func() {
		var tmpDir string
		var tmpLogPath string
		var origCwd string
		BeforeEach(func() {
			d, err := ioutil.TempDir(os.TempDir(), "conmon-")
			Expect(err).To(BeNil())
			tmpDir = d
			tmpLogPath = filepath.Join(tmpDir, "log")
			origCwd, err = os.Getwd()
			Expect(err).To(BeNil())
		})
		AfterEach(func() {
			for {
				// There is a race condition on the directory deletion
				// as conmon could still be running and creating files
				// under tmpDir.  Attempt rmdir again if it fails with
				// ENOTEMPTY.
				err := os.RemoveAll(tmpDir)
				if err != nil && errors.Is(err, unix.ENOTEMPTY) {
					continue
				}
				Expect(err).To(BeNil())
				break
			}
			Expect(os.RemoveAll(tmpDir)).To(BeNil())
			err := os.Chdir(origCwd)
			Expect(err).To(BeNil())
		})
		It("no log driver should fail", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
			)
			Expect(stderr).To(ContainSubstring("Log driver not provided. Use --log-path"))
		})
		It("empty log driver should fail", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogPath(""),
			)
			Expect(stderr).To(ContainSubstring("log-path must not be empty"))
		})
		It("empty log driver and path should fail", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogPath(":"),
			)
			Expect(stderr).To(ContainSubstring("log-path must not be empty"))
		})
		It("k8s-file requires a filename", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogPath("k8s-file"),
			)
			Expect(stderr).To(ContainSubstring("k8s-file requires a filename"))
		})
		It("k8s-file: requires a filename", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogPath("k8s-file:"),
			)
			Expect(stderr).To(ContainSubstring("k8s-file requires a filename"))
		})
		It("log driver as path should pass", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("", tmpLogPath),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat(tmpLogPath)
			Expect(err).To(BeNil())
		})
		It("log driver as k8s-file:path should pass", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("k8s-file", tmpLogPath),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat(tmpLogPath)
			Expect(err).To(BeNil())
		})
		It("log driver as :path should pass", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogPath(":"+tmpLogPath),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat(tmpLogPath)
			Expect(err).To(BeNil())
		})
		It("log driver as none should pass", func() {
			direrr := os.Chdir(tmpDir)
			Expect(direrr).To(BeNil())

			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("none", ""),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat("none")
			Expect(err).NotTo(BeNil())
		})
		It("log driver as off should pass", func() {
			direrr := os.Chdir(tmpDir)
			Expect(direrr).To(BeNil())

			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("off", ""),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat("off")
			Expect(err).NotTo(BeNil())
		})
		It("log driver as null should pass", func() {
			direrr := os.Chdir(tmpDir)
			Expect(direrr).To(BeNil())

			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("null", ""),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat("none")
			Expect(err).NotTo(BeNil())
		})
		It("log driver as journald should pass", func() {
			direrr := os.Chdir(tmpDir)
			Expect(direrr).To(BeNil())

			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("journald", ""),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat("journald")
			Expect(err).NotTo(BeNil())
		})
		It("log driver as :journald should pass", func() {
			direrr := os.Chdir(tmpDir)
			Expect(direrr).To(BeNil())

			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogPath(":journald"),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat("journald")
			Expect(err).To(BeNil())
		})
		It("log driver as journald with short cid should fail", func() {
			// conmon requires a cid of len > 12
			shortCtrID := "abcdefghijkl"
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(shortCtrID),
				conmon.WithContainerUUID(shortCtrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("journald", ""),
			)
			Expect(stderr).To(ContainSubstring("Container ID must be longer than 12 characters"))
		})
		It("log driver as k8s-file with path should pass", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("k8s-file", tmpLogPath),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat(tmpLogPath)
			Expect(err).To(BeNil())
		})
		It("log driver as k8s-file with invalid path should fail", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("k8s-file", invalidPath),
			)
			Expect(stderr).To(ContainSubstring("Failed to open log file"))
		})
		It("log driver as invalid driver should fail", func() {
			invalidLogDriver := "invalid"
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver(invalidLogDriver, tmpLogPath),
			)
			Expect(stderr).To(ContainSubstring("No such log driver " + invalidLogDriver))
		})
		It("log driver as invalid driver with a blank path should fail", func() {
			invalidLogDriver := "invalid"
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver(invalidLogDriver, ""),
			)
			Expect(stderr).To(ContainSubstring("No such log driver " + invalidLogDriver))
		})
		It("multiple log drivers should pass", func() {
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("k8s-file", tmpLogPath),
				conmon.WithLogDriver("journald", ""),
			)
			Expect(stderr).To(BeEmpty())

			_, err := os.Stat(tmpLogPath)
			Expect(err).To(BeNil())
		})
		It("multiple log drivers with one invalid should fail", func() {
			invalidLogDriver := "invalid"
			_, stderr := getConmonOutputGivenOptions(
				conmon.WithPath(conmonPath),
				conmon.WithContainerID(ctrID),
				conmon.WithContainerUUID(ctrID),
				conmon.WithRuntimePath(validPath),
				conmon.WithLogDriver("k8s-file", tmpLogPath),
				conmon.WithLogDriver(invalidLogDriver, tmpLogPath),
			)
			Expect(stderr).To(ContainSubstring("No such log driver " + invalidLogDriver))
		})
	})
})
