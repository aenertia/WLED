## docs(lwip): warn that LWIP_ENABLE_LCP_ECHO causes PPP link termination under load

**Forgejo**: Fixes #21

**Target repo**: espressif/esp-idf
**WLED workaround**: sdkconfig.defaults — CONFIG_LWIP_ENABLE_LCP_ECHO=n

### Problem

`LWIP_ENABLE_LCP_ECHO` sends LCP echo-requests to verify the PPP link is
alive. Under sustained DDP flood load, the PPP RX task is starved — it
can't process LCP echo-replies fast enough. The LCP echo timeout fires and
terminates the PPP link, even though the link is physically fine.

The Kconfig help text for `LWIP_ENABLE_LCP_ECHO` doesn't mention this
failure mode.

### Proposed fix (for espressif/esp-idf)

In `components/lwip/Kconfig`, add to the `LWIP_ENABLE_LCP_ECHO` help text:

```diff
 config LWIP_ENABLE_LCP_ECHO
   bool "Enable LCP ECHO"
   default n
   help
     Enable LCP echo-request/reply to detect dead PPP links.
+    Note: under sustained high-throughput load (e.g. DDP pixel streaming),
+    the PPP RX task may be starved and unable to process echo-replies in
+    time. This causes spurious link termination even when the physical link
+    is healthy. Disable this option if you experience unexpected PPP
+    disconnects under load.
```

### WLED workaround

`sdkconfig.defaults` sets `CONFIG_LWIP_ENABLE_LCP_ECHO=n` to avoid this.