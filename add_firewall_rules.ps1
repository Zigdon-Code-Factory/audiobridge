$rules = @(
    @{
        Name = "AudioBridge APK Server"
        Protocol = "TCP"
        LocalPort = 8081
    }
    @{
        Name = "AudioBridge Discovery"
        Protocol = "UDP"
        LocalPort = 4011
    }
    @{
        Name = "AudioBridge Stream"
        Protocol = "UDP"
        LocalPort = 4012
    }
)

foreach ($rule in $rules) {
    netsh advfirewall firewall delete rule name="$($rule.Name)" | Out-Null
    netsh advfirewall firewall add rule `
        name="$($rule.Name)" `
        dir=in `
        action=allow `
        protocol=$($rule.Protocol) `
        localport=$($rule.LocalPort) `
        remoteip=localsubnet
}
