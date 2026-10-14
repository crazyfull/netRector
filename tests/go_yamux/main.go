// Interop helper built on the original hashicorp/yamux, used by tests/run_tests.py
// (--extserver) to check protocol compatibility of the C++ client and server.
//
//   server: goyamux --listen 127.0.0.1:9191 [--tls-cert c --tls-key k]
//   client: goyamux --client --server 127.0.0.1:9191 --listen 127.0.0.1:1080 [--insecure]
package main

import (
	"crypto/tls"
	"encoding/binary"
	"errors"
	"flag"
	"io"
	"log"
	"net"
	"strconv"
	"syscall"
	"time"

	"github.com/hashicorp/yamux"
)

func main() {
	listen := flag.String("listen", "127.0.0.1:9191", "")
	cert := flag.String("tls-cert", "", "")
	key := flag.String("tls-key", "", "")
	flag.Bool("v", false, "")
	client := flag.Bool("client", false, "")
	server := flag.String("server", "", "")
	insecure := flag.Bool("insecure", false, "")
	flag.Parse()

	if *client {
		runClient(*listen, *server, *insecure)
		return
	}

	var ln net.Listener
	var err error
	if *cert != "" {
		c, e := tls.LoadX509KeyPair(*cert, *key)
		if e != nil {
			log.Fatal(e)
		}
		ln, err = tls.Listen("tcp", *listen, &tls.Config{Certificates: []tls.Certificate{c}})
	} else {
		ln, err = net.Listen("tcp", *listen)
	}
	if err != nil {
		log.Fatal(err)
	}
	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Fatal(err)
		}
		go handleSession(conn)
	}
}

func handleSession(conn net.Conn) {
	cfg := yamux.DefaultConfig()
	cfg.KeepAliveInterval = 2 * time.Second // exercise ping handling
	sess, err := yamux.Server(conn, cfg)
	if err != nil {
		log.Println("session:", err)
		return
	}
	defer sess.Close()
	for {
		st, err := sess.AcceptStream()
		if err != nil {
			log.Println("session closed:", err)
			return
		}
		go handleStream(st)
	}
}

func reply(st *yamux.Stream, rep byte) {
	st.Write([]byte{5, rep, 0, 1, 0, 0, 0, 0, 0, 0})
}

func handleStream(st *yamux.Stream) {
	defer st.Close()
	hdr := make([]byte, 4)
	if _, err := io.ReadFull(st, hdr); err != nil {
		return
	}
	var host string
	switch hdr[3] {
	case 1:
		b := make([]byte, 4)
		if _, err := io.ReadFull(st, b); err != nil {
			return
		}
		host = net.IP(b).String()
	case 4:
		b := make([]byte, 16)
		if _, err := io.ReadFull(st, b); err != nil {
			return
		}
		host = net.IP(b).String()
	case 3:
		l := make([]byte, 1)
		if _, err := io.ReadFull(st, l); err != nil {
			return
		}
		b := make([]byte, int(l[0]))
		if _, err := io.ReadFull(st, b); err != nil {
			return
		}
		host = string(b)
	default:
		reply(st, 8)
		return
	}
	pb := make([]byte, 2)
	if _, err := io.ReadFull(st, pb); err != nil {
		return
	}
	port := binary.BigEndian.Uint16(pb)

	dst, err := net.DialTimeout("tcp", net.JoinHostPort(host, strconv.Itoa(int(port))), 15*time.Second)
	if err != nil {
		if errors.Is(err, syscall.ECONNREFUSED) {
			reply(st, 5)
		} else {
			reply(st, 4)
		}
		return
	}
	defer dst.Close()
	reply(st, 0)

	done := make(chan struct{}, 2)
	go func() {
		io.Copy(dst, st)
		if tc, ok := dst.(*net.TCPConn); ok {
			tc.CloseWrite()
		}
		done <- struct{}{}
	}()
	go func() {
		io.Copy(st, dst)
		st.Close() // FIN (half-close)
		done <- struct{}{}
	}()
	<-done
	<-done
}

// ---- client mode: local SOCKS5 -> one hashicorp/yamux stream per connection ----

func runClient(listen, server string, useTLS bool) {
	var conn net.Conn
	var err error
	for i := 0; i < 100; i++ {
		if useTLS {
			conn, err = tls.Dial("tcp", server, &tls.Config{InsecureSkipVerify: true})
		} else {
			conn, err = net.Dial("tcp", server)
		}
		if err == nil {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if err != nil {
		log.Fatal(err)
	}
	cfg := yamux.DefaultConfig()
	cfg.KeepAliveInterval = 2 * time.Second
	sess, err := yamux.Client(conn, cfg)
	if err != nil {
		log.Fatal(err)
	}
	ln, err := net.Listen("tcp", listen)
	if err != nil {
		log.Fatal(err)
	}
	for {
		c, err := ln.Accept()
		if err != nil {
			log.Fatal(err)
		}
		go handleLocal(sess, c.(*net.TCPConn))
	}
}

func handleLocal(sess *yamux.Session, c *net.TCPConn) {
	defer c.Close()
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(c, hdr); err != nil {
		return
	}
	methods := make([]byte, int(hdr[1]))
	if _, err := io.ReadFull(c, methods); err != nil {
		return
	}
	c.Write([]byte{5, 0})

	st, err := sess.OpenStream()
	if err != nil {
		c.Write([]byte{5, 1, 0, 1, 0, 0, 0, 0, 0, 0})
		return
	}
	defer st.Close()

	done := make(chan struct{}, 2)
	go func() {
		io.Copy(st, c) // request + data
		st.Close()     // FIN
		done <- struct{}{}
	}()
	go func() {
		io.Copy(c, st) // reply + data
		c.CloseWrite()
		done <- struct{}{}
	}()
	<-done
	<-done
}
