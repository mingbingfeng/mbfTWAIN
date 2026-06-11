using System;
using System.Threading;
using System.Windows.Forms;

namespace MbfTwain.VirtualScannerConfig;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        DiagnosticsLog.Write("UI", $"Process start logPath={DiagnosticsLog.PathOnDisk}");
        Application.SetUnhandledExceptionMode(UnhandledExceptionMode.CatchException);
        Application.ThreadException += (_, args) =>
        {
            DiagnosticsLog.WriteException("UI", args.Exception, "Application.ThreadException");
        };
        AppDomain.CurrentDomain.UnhandledException += (_, args) =>
        {
            if (args.ExceptionObject is Exception exception)
            {
                DiagnosticsLog.WriteException("UI", exception, "AppDomain.CurrentDomain.UnhandledException");
            }
            else
            {
                DiagnosticsLog.Write("UI", $"Unhandled exception object: {args.ExceptionObject}");
            }
        };

        ApplicationConfiguration.Initialize();
        try
        {
            Application.Run(new MainForm());
            DiagnosticsLog.Write("UI", "Application.Run exited normally");
        }
        catch (Exception exception)
        {
            DiagnosticsLog.WriteException("UI", exception, "Application.Run");
            throw;
        }
    }
}
