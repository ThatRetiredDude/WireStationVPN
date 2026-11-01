package main

// https for the PS5's own tailnet, so every Tailscale app accepts the join
// address (some phones refuse plain http). The certificate comes from:
//
//   - Let's Encrypt (free), for the DuckDNS home name (ddns.go). It proves
//     the name is ours with a DNS record set through DuckDNS (the "DNS-01"
//     check), so no extra port has to be forwarded. Renewed by itself.
//   - or files of your own: tls_cert and tls_key.
//
// The front door then answers https and, for older setups and the PS5's own
// node, plain http on the same port.

import (
	"bufio"
	"context"
	"crypto"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"time"

	"golang.org/x/crypto/acme"
)

// acmeDirectory: Let's Encrypt. The test points it at its own.
var acmeDirectory = "https://acme-v02.api.letsencrypt.org/directory"

// acmeHTTP: how it talks to Let's Encrypt; the test's server has its own
// certificate.
var acmeHTTP = http.DefaultClient

// txtSeen waits until the world can see the TXT record (or gives up and
// lets Let's Encrypt try); the test skips it.
var txtSeen = waitForTXT

type certSource struct {
	mu   sync.Mutex
	cert *tls.Certificate
	name string // what it's for, for the status page
}

func (c *certSource) get(*tls.ClientHelloInfo) (*tls.Certificate, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.cert == nil {
		return nil, errors.New("no certificate yet")
	}
	return c.cert, nil
}

func (c *certSource) set(cert *tls.Certificate) {
	c.mu.Lock()
	c.cert = cert
	c.mu.Unlock()
}

// expires: when the certificate stops being valid.
func (c *certSource) expires() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.cert == nil || c.cert.Leaf == nil {
		return time.Time{}
	}
	return c.cert.Leaf.NotAfter
}

// startTLS: the certificate for the front door, or nil when https is off or
// there is nothing to get one for. A first certificate from Let's Encrypt
// takes a minute or two.
func startTLS(ctx context.Context, dir string, cfg settings, log *logger) (*certSource, error) {
	if !cfg.HTTPS {
		return nil, nil
	}
	if cfg.TLSCert != "" {
		cert, err := loadCert(cfg.TLSCert, cfg.TLSKey)
		if err != nil {
			return nil, fmt.Errorf("tls_cert and tls_key: %w", err)
		}
		c := &certSource{name: "your own certificate"}
		c.set(cert)
		go func() { // pick up a replaced file
			for ctx.Err() == nil {
				select {
				case <-ctx.Done():
				case <-time.After(time.Hour):
					if cert, err := loadCert(cfg.TLSCert, cfg.TLSKey); err == nil {
						c.set(cert)
					}
				}
			}
		}()
		return c, nil
	}
	if cfg.DuckDomain == "" || cfg.DuckToken == "" {
		return nil, nil
	}
	name := duckName(cfg.DuckDomain)
	tlsDir := filepath.Join(dir, "headscale", "tls")
	if err := os.MkdirAll(tlsDir, 0o700); err != nil {
		return nil, err
	}
	certPath, keyPath := filepath.Join(tlsDir, "cert.pem"), filepath.Join(tlsDir, "key.pem")
	c := &certSource{name: "Let's Encrypt, for " + name}
	renew := func() error {
		certPEM, keyPEM, err := obtainCert(ctx, tlsDir, name, cfg, log)
		if err != nil {
			return err
		}
		if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
			return err
		}
		if err := os.WriteFile(certPath, certPEM, 0o600); err != nil {
			return err
		}
		cert, err := loadCert(certPath, keyPath)
		if err != nil {
			return err
		}
		c.set(cert)
		log.Say("https: certificate for %s until %s", name, cert.Leaf.NotAfter.Format("2 Jan 2006"))
		return nil
	}
	if cert, err := loadCert(certPath, keyPath); err == nil && cert.Leaf.Subject.CommonName == name ||
		err == nil && len(cert.Leaf.DNSNames) > 0 && cert.Leaf.DNSNames[0] == name {
		c.set(cert)
	}
	if time.Until(c.expires()) < 30*24*time.Hour {
		if err := renew(); err != nil && c.expires().Before(time.Now()) {
			return nil, fmt.Errorf("no https certificate from Let's Encrypt: %w", err)
		} else if err != nil {
			log.Say("https: renewing failed, the current certificate still works: %v", err)
		}
	}
	go func() {
		for {
			select {
			case <-ctx.Done():
				return
			case <-time.After(12 * time.Hour):
			}
			if time.Until(c.expires()) < 30*24*time.Hour {
				if err := renew(); err != nil {
					log.Say("https: renewing failed (will try again): %v", err)
				}
			}
		}
	}()
	return c, nil
}

func loadCert(certPath, keyPath string) (*tls.Certificate, error) {
	cert, err := tls.LoadX509KeyPair(certPath, keyPath)
	if err != nil {
		return nil, err
	}
	if cert.Leaf == nil {
		if cert.Leaf, err = x509.ParseCertificate(cert.Certificate[0]); err != nil {
			return nil, err
		}
	}
	return &cert, nil
}

// obtainCert gets a certificate for name from Let's Encrypt, proving the
// name with a TXT record set through DuckDNS.
func obtainCert(ctx context.Context, tlsDir, name string, cfg settings, log *logger) (certPEM, keyPEM []byte, err error) {
	ctx, cancel := context.WithTimeout(ctx, 6*time.Minute)
	defer cancel()
	log.Say("https: asking Let's Encrypt for a certificate for %s", name)
	accountKey, err := loadOrMakeKey(filepath.Join(tlsDir, "account.key"))
	if err != nil {
		return nil, nil, err
	}
	cl := &acme.Client{Key: accountKey, DirectoryURL: acmeDirectory, HTTPClient: acmeHTTP}
	acct := &acme.Account{}
	if cfg.ACMEEmail != "" {
		acct.Contact = []string{"mailto:" + cfg.ACMEEmail}
	}
	if _, err := cl.Register(ctx, acct, acme.AcceptTOS); err != nil && !errors.Is(err, acme.ErrAccountAlreadyExists) {
		return nil, nil, fmt.Errorf("Let's Encrypt account: %w", err)
	}
	order, err := cl.AuthorizeOrder(ctx, acme.DomainIDs(name))
	if err != nil {
		return nil, nil, fmt.Errorf("order: %w", err)
	}
	defer duckUpdate(context.WithoutCancel(ctx), cfg, "", true) // the TXT record isn't needed after
	for _, zurl := range order.AuthzURLs {
		z, err := cl.GetAuthorization(ctx, zurl)
		if err != nil {
			return nil, nil, err
		}
		if z.Status == acme.StatusValid {
			continue
		}
		var chal *acme.Challenge
		for _, ch := range z.Challenges {
			if ch.Type == "dns-01" {
				chal = ch
			}
		}
		if chal == nil {
			return nil, nil, errors.New("Let's Encrypt offered no DNS check")
		}
		value, err := cl.DNS01ChallengeRecord(chal.Token)
		if err != nil {
			return nil, nil, err
		}
		if err := duckUpdate(ctx, cfg, value, false); err != nil {
			return nil, nil, err
		}
		txtSeen(ctx, "_acme-challenge."+name, value)
		if _, err := cl.Accept(ctx, chal); err != nil {
			return nil, nil, fmt.Errorf("accept: %w", err)
		}
		if _, err := cl.WaitAuthorization(ctx, zurl); err != nil {
			return nil, nil, fmt.Errorf("Let's Encrypt's check of %s: %w", name, err)
		}
	}
	finalize, orderURI := order.FinalizeURL, order.URI
	if order, err = cl.WaitOrder(ctx, orderURI); err != nil {
		return nil, nil, fmt.Errorf("the order: %w", err)
	}
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	csr, err := x509.CreateCertificateRequest(rand.Reader, &x509.CertificateRequest{DNSNames: []string{name}}, key)
	if err != nil {
		return nil, nil, err
	}
	if order.FinalizeURL != "" {
		finalize = order.FinalizeURL
	}
	der, _, err := cl.CreateCertFromOrder(ctx, &acme.Order{URI: orderURI, FinalizeURL: finalize}, csr, true)
	if err != nil {
		return nil, nil, fmt.Errorf("certificate: %w", err)
	}
	for _, b := range der {
		certPEM = append(certPEM, pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: b})...)
	}
	kb, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return nil, nil, err
	}
	return certPEM, pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: kb}), nil
}

func loadOrMakeKey(path string) (crypto.Signer, error) {
	if b, err := os.ReadFile(path); err == nil {
		if blk, _ := pem.Decode(b); blk != nil {
			return x509.ParseECPrivateKey(blk.Bytes)
		}
	}
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}
	kb, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return nil, err
	}
	return key, os.WriteFile(path, pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: kb}), 0o600)
}

// waitForTXT: up to 3 minutes for a public DNS server to show the record.
func waitForTXT(ctx context.Context, name, value string) {
	r := &net.Resolver{PreferGo: true, Dial: func(ctx context.Context, _, _ string) (net.Conn, error) {
		return (&net.Dialer{}).DialContext(ctx, "udp", "1.1.1.1:53")
	}}
	for end := time.Now().Add(3 * time.Minute); time.Now().Before(end); {
		lctx, cancel := context.WithTimeout(ctx, 5*time.Second)
		txts, _ := r.LookupTXT(lctx, name)
		cancel()
		for _, t := range txts {
			if t == value {
				time.Sleep(10 * time.Second) // the other DNS servers too
				return
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(10 * time.Second):
		}
	}
}

// sniffListener: https and plain http on one port. A TLS connection starts
// with byte 0x16; anything else is plain http.
type sniffListener struct {
	net.Listener
	tls *tls.Config
}

func (l *sniffListener) Accept() (net.Conn, error) {
	c, err := l.Listener.Accept()
	if err != nil {
		return nil, err
	}
	return &sniffConn{Conn: c, cfg: l.tls}, nil
}

// sniffConn decides on its first Read, so a slow client can't hold up
// Accept for everyone else.
type sniffConn struct {
	net.Conn
	cfg  *tls.Config
	once sync.Once
	r    interface{ Read([]byte) (int, error) }
	tc   *tls.Conn
}

func (c *sniffConn) decide() {
	br := bufio.NewReader(c.Conn)
	c.Conn.SetReadDeadline(time.Now().Add(10 * time.Second))
	first, err := br.Peek(1)
	c.Conn.SetReadDeadline(time.Time{})
	pre := &prefixConn{Conn: c.Conn, r: br}
	if err == nil && first[0] == 0x16 {
		c.tc = tls.Server(pre, c.cfg)
		c.r = c.tc
		return
	}
	c.r = pre
}

func (c *sniffConn) Read(b []byte) (int, error) {
	c.once.Do(c.decide)
	return c.r.Read(b)
}

func (c *sniffConn) Write(b []byte) (int, error) {
	c.once.Do(c.decide)
	if c.tc != nil {
		return c.tc.Write(b)
	}
	return c.Conn.Write(b)
}

func (c *sniffConn) Close() error {
	if c.tc != nil {
		return c.tc.Close()
	}
	return c.Conn.Close()
}

type prefixConn struct {
	net.Conn
	r *bufio.Reader
}

func (p *prefixConn) Read(b []byte) (int, error) { return p.r.Read(b) }
