// AetherPrint — print_pdf on Android: the system's print dialog over a PDF
// file. PrintManager.print takes a PrintDocumentAdapter, an abstract class,
// so this is one: it reports the file as a document of unknown page count
// (the print service reads the PDF itself) and, when asked to write, copies
// the file's bytes to the destination it is given, as the platform's own
// PDF printing does.
package dev.aether.ui;

import android.app.Activity;
import android.content.Context;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.print.PageRange;
import android.print.PrintAttributes;
import android.print.PrintDocumentAdapter;
import android.print.PrintDocumentInfo;
import android.print.PrintManager;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;

final class AetherPrint extends PrintDocumentAdapter {
    private final String path, name;

    private AetherPrint(String path, String name) { this.path = path; this.name = name; }

    // True when the dialog was asked for (the file exists and the system
    // has a PrintManager).
    static boolean print(Activity activity, String path, String title) {
        if (activity == null || path == null || !new File(path).isFile()) return false;
        PrintManager pm = (PrintManager) activity.getSystemService(Context.PRINT_SERVICE);
        if (pm == null) return false;
        String job = title == null || title.isEmpty() ? new File(path).getName() : title;
        pm.print(job, new AetherPrint(path, job), null);
        return true;
    }

    @Override public void onLayout(PrintAttributes oldAttributes, PrintAttributes newAttributes,
                                   CancellationSignal cancel, LayoutResultCallback callback, Bundle extras) {
        if (cancel.isCanceled()) { callback.onLayoutCancelled(); return; }
        PrintDocumentInfo info = new PrintDocumentInfo.Builder(name.endsWith(".pdf") ? name : name + ".pdf")
            .setContentType(PrintDocumentInfo.CONTENT_TYPE_DOCUMENT)
            .setPageCount(PrintDocumentInfo.PAGE_COUNT_UNKNOWN)
            .build();
        callback.onLayoutFinished(info, !newAttributes.equals(oldAttributes));
    }

    @Override public void onWrite(PageRange[] pages, ParcelFileDescriptor destination,
                                  CancellationSignal cancel, WriteResultCallback callback) {
        try (InputStream in = new FileInputStream(path);
             OutputStream out = new FileOutputStream(destination.getFileDescriptor())) {
            byte[] buf = new byte[16384];
            int n;
            while ((n = in.read(buf)) > 0) {
                if (cancel.isCanceled()) { callback.onWriteCancelled(); return; }
                out.write(buf, 0, n);
            }
            callback.onWriteFinished(new PageRange[] { PageRange.ALL_PAGES });
        } catch (Exception e) {
            callback.onWriteFailed(e.toString());
        }
    }
}
