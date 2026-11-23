package main

import (
	"os"
)

func main() {
	log := newLogger(os.Stdout)
	if err := run(log); err != nil {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}
