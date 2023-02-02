package conmon_test

import (
	"bytes"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"testing"

	"github.com/containers/conmon/runner/conmon"
	"github.com/coreos/go-systemd/v22/sdjournal"
	. "github.com/onsi/ginkgo/v2"
	. "github.com/onsi/gomega"
)

var (
	conmonPath     = "/usr/bin/conmon"
	runtimePath    = "/usr/bin/runc"
	busyboxSource  = "https://busybox.net/downloads/binaries/1.31.0-i686-uclibc/busybox"
	busyboxDestDir = "/tmp/conmon-test-images"
	busyboxDest    = "/tmp/conmon-test-images/busybox"
	ctrID          = "abcdefghijklm"
	validPath      = "/tmp"
	invalidPath    = "/not/a/path"
	skopeoPath     = "/usr/bin/skopeo"
)

func TestConmon(t *testing.T) {
	configureSuiteFromEnv()
	RegisterFailHandler(Fail)
	RunSpecs(t, "Conmon Suite")
}

func getConmonOutputGivenOptions(options ...conmon.ConmonOption) (string, string) {
	var stdout bytes.Buffer
	var stderr bytes.Buffer
	var stdin bytes.Buffer

	options = append(options, conmon.WithStdout(&stdout), conmon.WithStderr(&stderr), conmon.WithStdin(&stdin))

	ci, err := conmon.CreateAndExecConmon(options...)
	Expect(err).To(BeNil())

	defer ci.Cleanup()

	ci.Wait()

	pid, _ := ci.Pid()
	if pid < 0 {
		return stdout.String(), stderr.String()
	}

	_, err = ci.ContainerExitCode()
	Expect(err).To(BeNil())

	journalerr, err := getConmonJournalOutput(pid, 3)
	Expect(err).To(BeNil())

	alljournalout, err := getConmonJournalOutput(pid, -1)
	Expect(err).To(BeNil())
	fmt.Fprintf(GinkgoWriter, alljournalout+"\n")

	return stdout.String(), stderr.String() + journalerr
}

func getConmonJournalOutput(pid int, level int) (string, error) {
	matches := []sdjournal.Match{
		{
			Field: sdjournal.SD_JOURNAL_FIELD_COMM,
			Value: "conmon",
		},
		{
			Field: sdjournal.SD_JOURNAL_FIELD_PID,
			Value: strconv.Itoa(pid),
		},
	}
	if level > 0 {
		matches = append(matches, sdjournal.Match{
			Field: sdjournal.SD_JOURNAL_FIELD_PRIORITY,
			Value: strconv.Itoa(level),
		})
	}
	r, err := sdjournal.NewJournalReader(sdjournal.JournalReaderConfig{
		Matches:   matches,
		Formatter: formatter,
	})
	if err != nil {
		return "", err
	}
	defer r.Close()

	return readAllFromBuffer(r)
}

func formatter(entry *sdjournal.JournalEntry) (string, error) {
	return entry.Fields[sdjournal.SD_JOURNAL_FIELD_MESSAGE], nil
}

func readAllFromBuffer(r io.ReadCloser) (string, error) {
	bufLen := 16384
	stringOutput := ""

	bytes := make([]byte, bufLen)
	// /me complains about no do-while in go
	ec, err := r.Read(bytes)
	for ec != 0 && err == nil {
		// because we are reusing bytes, we need to make
		// sure the old data doesn't get into the new line
		bytestr := string(bytes[:ec])
		stringOutput += string(bytestr)
		ec, err = r.Read(bytes)
	}
	if err != nil && err != io.EOF {
		return stringOutput, err
	}
	return stringOutput, nil
}

func configureSuiteFromEnv() {
	if path := os.Getenv("CONMON_BINARY"); path != "" {
		conmonPath = path
	}
	if path := os.Getenv("RUNTIME_BINARY"); path != "" {
		runtimePath = path
	}
}

func cacheBusyBox() error {
	if _, err := os.Stat(busyboxDest); err == nil {
		return nil
	}
	if err := os.MkdirAll(busyboxDestDir, 0755); err != nil && !os.IsExist(err) {
		return err
	}
	if err := downloadFile(busyboxSource, busyboxDest); err != nil {
		return err
	}
	if err := os.Chmod(busyboxDest, 0777); err != nil {
		return err
	}
	return nil
}

// source: https://progolang.com/how-to-download-files-in-go/
// downloadFile will download a url and store it in local filepath.
// It writes to the destination file as it downloads it, without
// loading the entire file into memory.
func downloadFile(url string, filepath string) error {
	// Create the file
	out, err := os.Create(filepath)
	if err != nil {
		return err
	}
	defer out.Close()

	// Get the data
	resp, err := http.Get(url)
	if err != nil {
		return err
	}
	defer resp.Body.Close()

	// Write the body to file
	_, err = io.Copy(out, resp.Body)
	if err != nil {
		return err
	}

	return nil
}

func runRuntimeCommand(args ...string) error {
	var stdout bytes.Buffer
	var stderr bytes.Buffer

	cmd := exec.Command(runtimePath, args...)
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	if err := cmd.Run(); err != nil {
		return err
	}
	cmd.Run()
	stdoutString := stdout.String()
	if stdoutString != "" {
		fmt.Fprintf(GinkgoWriter, stdoutString+"\n")
	}
	return nil
}
