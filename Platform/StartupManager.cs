using System;
using System.Diagnostics;
using System.IO;
using System.Security;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace Mmoni.Platform;

internal static class StartupManager
{
    internal const string TaskName = "Mmoni";
    private const string LegacyRunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string LegacyRunValueName = "Mmoni";

    public static bool IsEnabled()
    {
        var (exitCode, _, _) = RunSchtasks($"/Query /TN \"{TaskName}\"");
        return exitCode == 0;
    }

    public static void SetEnabled(bool enabled)
    {
        RemoveLegacyRunEntry();

        if (enabled)
            CreateScheduledTask();
        else
            DeleteScheduledTask();
    }

    public static void MigrateLegacyStartupIfNeeded()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(LegacyRunKey);
            if (key?.GetValue(LegacyRunValueName) is not string)
                return;

            SetEnabled(true);
        }
        catch (Exception ex)
        {
            Trace.TraceWarning("Mmoni could not migrate legacy startup entry: {0}", ex.Message);
        }
    }

    private static void CreateScheduledTask()
    {
        var executable = Environment.ProcessPath
            ?? throw new InvalidOperationException("Executable path unavailable.");
        var workingDirectory = Path.GetDirectoryName(executable)
            ?? throw new InvalidOperationException("Executable directory unavailable.");

        using var identity = WindowsIdentity.GetCurrent();
        var userSid = identity.User?.Value
            ?? throw new InvalidOperationException("Current user SID unavailable.");

        var xml = BuildTaskXml(executable, workingDirectory, userSid);
        var tempXmlPath = Path.Combine(Path.GetTempPath(), $"Mmoni-Task-{Guid.NewGuid():N}.xml");

        try
        {
            File.WriteAllText(tempXmlPath, xml, Encoding.Unicode);
            var (exitCode, _, stderr) = RunSchtasks($"/Create /TN \"{TaskName}\" /XML \"{tempXmlPath}\" /F");
            if (exitCode != 0)
            {
                var detail = string.IsNullOrWhiteSpace(stderr) ? $"schtasks exited with code {exitCode}." : stderr.Trim();
                throw new InvalidOperationException(detail);
            }
        }
        finally
        {
            try
            {
                if (File.Exists(tempXmlPath))
                    File.Delete(tempXmlPath);
            }
            catch (IOException)
            {
            }
        }
    }

    private static void DeleteScheduledTask()
    {
        var (exitCode, _, stderr) = RunSchtasks($"/Delete /TN \"{TaskName}\" /F");
        if (exitCode != 0 && !IsTaskNotFound(exitCode, stderr))
        {
            var detail = string.IsNullOrWhiteSpace(stderr) ? $"schtasks exited with code {exitCode}." : stderr.Trim();
            throw new InvalidOperationException(detail);
        }
    }

    private static void RemoveLegacyRunEntry()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(LegacyRunKey, writable: true);
            key?.DeleteValue(LegacyRunValueName, throwOnMissingValue: false);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or SecurityException)
        {
            Trace.TraceWarning("Could not remove legacy Run registry entry: {0}", ex.Message);
        }
    }

    private static bool IsTaskNotFound(int exitCode, string stderr)
    {
        if (exitCode == 0)
            return false;

        // Verify by querying: if query also fails, the task does not exist.
        var (queryExitCode, _, _) = RunSchtasks($"/Query /TN \"{TaskName}\"");
        return queryExitCode != 0 || stderr.Contains("cannot find", StringComparison.OrdinalIgnoreCase);
    }

    private static string BuildTaskXml(string executablePath, string workingDirectory, string userSid)
    {
        var escapedExe = SecurityElement.Escape(executablePath);
        var escapedDir = SecurityElement.Escape(workingDirectory);
        var escapedSid = SecurityElement.Escape(userSid);

        return $"""
            <?xml version="1.0" encoding="UTF-16"?>
            <Task version="1.3" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
              <RegistrationInfo>
                <Description>Starts Mmoni hardware telemetry overlay with administrator privileges at user sign-in.</Description>
                <URI>\{TaskName}</URI>
              </RegistrationInfo>
              <Triggers>
                <LogonTrigger>
                  <Enabled>true</Enabled>
                  <UserId>{escapedSid}</UserId>
                  <Delay>PT3S</Delay>
                </LogonTrigger>
              </Triggers>
              <Principals>
                <Principal id="Author">
                  <UserId>{escapedSid}</UserId>
                  <LogonType>InteractiveToken</LogonType>
                  <RunLevel>HighestAvailable</RunLevel>
                </Principal>
              </Principals>
              <Settings>
                <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
                <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
                <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
                <AllowHardTerminate>false</AllowHardTerminate>
                <StartWhenAvailable>true</StartWhenAvailable>
                <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>
                <IdleSettings>
                  <StopOnIdleEnd>false</StopOnIdleEnd>
                  <RestartOnIdle>false</RestartOnIdle>
                </IdleSettings>
                <AllowStartOnDemand>true</AllowStartOnDemand>
                <Enabled>true</Enabled>
                <Hidden>false</Hidden>
                <RunOnlyIfIdle>false</RunOnlyIfIdle>
                <WakeToRun>false</WakeToRun>
                <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
                <Priority>5</Priority>
              </Settings>
              <Actions Context="Author">
                <Exec>
                  <Command>"{escapedExe}"</Command>
                  <WorkingDirectory>{escapedDir}</WorkingDirectory>
                </Exec>
              </Actions>
            </Task>
            """;
    }

    private static (int ExitCode, string StdOut, string StdErr) RunSchtasks(string arguments)
    {
        var schtasksPath = Path.Combine(Environment.SystemDirectory, "schtasks.exe");
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = schtasksPath,
                Arguments = arguments,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            }
        };

        process.Start();
        var stdout = process.StandardOutput.ReadToEnd();
        var stderr = process.StandardError.ReadToEnd();
        process.WaitForExit(10_000);
        return (process.ExitCode, stdout, stderr);
    }
}
