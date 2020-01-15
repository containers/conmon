package conmon_test

import (
	"testing"

	. "github.com/onsi/ginkgo"
	. "github.com/onsi/gomega"
)

func TestConmon(t *testing.T) {
	RegisterFailHandler(Fail)
	RunSpecs(t, "Conmon Suite")
}
