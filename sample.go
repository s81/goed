package main

import (
	"fmt"
	"time"
)

func main() {
	for i := 0; ; i++ {
		_ = i
		fmt.Println("tick", i)
		time.Sleep(500 * time.Millisecond)
	}
}
