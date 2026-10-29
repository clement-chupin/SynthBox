package com.grvep.simulator;

import android.app.Activity;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

// Subclasses SDLActivity solely to add SAF (Storage Access Framework) folder
// import: MODE_IMPORT in the native app (src/main.cpp) calls pickImportFolder()
// below via JNI (see androidPickFolder() in main.cpp) to let the user grant
// access to a real folder on their phone, then this copies its files onto the
// app's SD root (getExternalFilesDir(null) — the same path simulator/hal/SD.h
// resolves to on Android) so they behave exactly like SD-card files from then on.
//
// This is a one-time/re-triggerable import, not a live mount: SAF grants a
// content:// tree, and the rest of the firmware's SD.h shim expects plain
// filesystem paths, so bridging via a copy avoids reimplementing that whole
// file abstraction over content://.
//
// Progress is reported back to native not via a second JNI direction but by
// writing a small status file into the SD root that the native side polls with
// its own existing SD.open() (see the importPhase==1 block in main.cpp's loop()).
public class GrvActivity extends SDLActivity {
    private static final String TAG = "GrvActivity";
    private static final int REQ_PICK_FOLDER = 4242;

    // Called from native via JNI (androidPickFolder() in src/main.cpp).
    public void pickImportFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, REQ_PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != REQ_PICK_FOLDER || resultCode != Activity.RESULT_OK || data == null) return;
        final Uri treeUri = data.getData();
        if (treeUri == null) return;
        try {
            getContentResolver().takePersistableUriPermission(treeUri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            Log.w(TAG, "takePersistableUriPermission failed", e);
        }
        final File sdRoot = getExternalFilesDir(null);
        new Thread(new Runnable() {
            @Override public void run() { runImport(treeUri, sdRoot); }
        }, "grv-import").start();
    }

    private static class DocEntry {
        final Uri uri; final String relPath; final long size;
        DocEntry(Uri u, String p, long s) { uri = u; relPath = p; size = s; }
    }

    private void runImport(Uri treeUri, File sdRoot) {
        if (sdRoot == null) return;
        ContentResolver cr = getContentResolver();
        File statusFile = new File(sdRoot, ".grv_import.status");
        List<DocEntry> files = new ArrayList<>();
        Uri rootDocUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, DocumentsContract.getTreeDocumentId(treeUri));
        listFilesRecursive(cr, treeUri, rootDocUri, "", files);

        int total = files.size();
        writeStatus(statusFile, "RUNNING", 0, total);
        int done = 0;
        for (DocEntry entry : files) {
            File dest = new File(sdRoot, entry.relPath);
            // Cheap incremental sync: skip files already imported at the same size.
            if (!(dest.exists() && dest.length() == entry.size)) {
                copyOne(cr, entry.uri, dest);
            }
            done++;
            writeStatus(statusFile, "RUNNING", done, total);
        }
        writeStatus(statusFile, "DONE", total, total);
    }

    private void listFilesRecursive(ContentResolver cr, Uri treeUri, Uri dirUri, String relPrefix, List<DocEntry> out) {
        Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, DocumentsContract.getDocumentId(dirUri));
        Cursor c = null;
        try {
            c = cr.query(childrenUri, new String[]{
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE
            }, null, null, null);
            if (c == null) return;
            while (c.moveToNext()) {
                String docId = c.getString(0);
                String name = c.getString(1);
                String mime = c.getString(2);
                long size = c.getLong(3);
                Uri childUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
                String relPath = relPrefix.isEmpty() ? name : relPrefix + "/" + name;
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    listFilesRecursive(cr, treeUri, childUri, relPath, out);
                } else {
                    out.add(new DocEntry(childUri, relPath, size));
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "listFilesRecursive failed for " + dirUri, e);
        } finally {
            if (c != null) c.close();
        }
    }

    private void copyOne(ContentResolver cr, Uri src, File dest) {
        File parent = dest.getParentFile();
        if (parent != null && !parent.exists()) parent.mkdirs();
        InputStream in = null;
        OutputStream out = null;
        try {
            in = cr.openInputStream(src);
            if (in == null) return;
            out = new FileOutputStream(dest);
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        } catch (IOException e) {
            Log.w(TAG, "copyOne failed for " + src, e);
        } finally {
            try { if (in != null) in.close(); } catch (IOException ignored) {}
            try { if (out != null) out.close(); } catch (IOException ignored) {}
        }
    }

    private void writeStatus(File statusFile, String tag, int done, int total) {
        try {
            FileOutputStream fos = new FileOutputStream(statusFile);
            fos.write((tag + " " + done + " " + total + "\n").getBytes());
            fos.close();
        } catch (IOException e) {
            Log.w(TAG, "writeStatus failed", e);
        }
    }
}
