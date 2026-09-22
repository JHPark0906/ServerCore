# Suffix, probe selector, and the replacement required in the diagnostic.
# Shared by both consumer projects and the package-test driver.
set(SERVERCORE_DEPRECATED_API_PROBES
        "HttpRoute|1|RegisterRoute"
        "HttpWebSocket|2|RegisterWebSocket"
        "DatagramSend|3|SendSerialized"
        "JobRunnerStop|4|RequestStop")
