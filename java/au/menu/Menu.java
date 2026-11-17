package au.menu;

import android.animation.ValueAnimator;
import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.ViewParent;
import android.view.Window;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.util.Locale;

/**
 * Floating mod menu drawn with plain Android views (custom toggle and slider, no theme or
 * resources needed), so it looks the same on every Unity build and works on GLES and Vulkan.
 */
public final class Menu {
    private static final String TAG = "AUMod";
    private static final String VERSION = "v2.4";

    // Feature ids and status bits are shared with src/java_menu.cpp.
    private static final int SPEED = 0, LIGHT = 1, NOCLIP = 2, KILL = 3, ZOOM = 4, REVEAL = 5, GHOSTS = 6;
    private static final int ST_SPEED = 1, ST_NOCLIP = 2, ST_LIGHT = 4, ST_KILL = 8, ST_LOCAL = 16,
            ST_ZOOM = 32, ST_REVEAL = 64, ST_GHOSTS = 128;
    // Layout of nativeConfig(): speedOn, speedMul, lightOn, lightMul, noclip, killOn,
    // zoomOn, zoomMul, revealOn, ghostsOn.
    private static final int CFG_LEN = 10;
    // Must match kSpeedMul* / kLightMul* / kZoomMul* in src/common.h.
    private static final float SPEED_MIN = 0.5f, SPEED_MAX = 4f, LIGHT_MIN = 0.5f, LIGHT_MAX = 8f;
    private static final float ZOOM_MIN = 1f, ZOOM_MAX = 4f;
    private static final float SLIDER_STEP = 0.05f;
    private static final long STATUS_PERIOD_MS = 1000;
    // After attach the window is relaid out at these delays: on size-compat (letterboxed) windows the
    // drawn surface and the input/layout frame can disagree until the first relayoutWindow().
    private static final long[] RELAYOUT_DELAYS_MS = {0, 600, 2000};
    private static final long RELAYOUT_MIN_GAP_MS = 250;
    private static final int MAX_GEOMETRY_LOGS = 40;

    // Palette.
    private static final int BG = 0xF2181A20, SURFACE = 0xFF23262F, STROKE = 0x1FFFFFFF;
    private static final int TEXT = 0xFFF2F3F5, MUTED = 0xFF8D93A0, OFF = 0xFF3A3D47;
    private static final int ACCENT = 0xFFE5484D, ACCENT_2 = 0xFFFF8A3D, OK = 0xFF3DD68C;

    private static native void nativeSet(int id, boolean on, float value);
    private static native int nativeStatus();
    private static native float[] nativeConfig();
    private static native void nativeDump();
    private static native void nativeLog(String msg);

    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private static WeakReference<Activity> attachedTo = new WeakReference<Activity>(null);
    private static boolean callbacksRegistered;
    private static boolean polling;

    private static FrameLayout root;
    private static TextView fab;
    private static LinearLayout panel;
    private static TextView statusPill;
    private static FeatureRow speedRow, lightRow, noclipRow, killRow, zoomRow, revealRow, ghostsRow;
    private static boolean panelOpen;
    private static int lastStatus = -1;
    // Bumped on every attach, so delayed work posted for an old overlay does nothing.
    private static int generation;
    private static long lastRelayoutAt;
    private static int geometryLogs;
    // FAB position as a fraction of the free area, kept across activity re-attach.
    private static float fabFx = 0f, fabFy = 0.3f;

    private Menu() {}

    // ---------------------------------------------------------------- lifecycle

    /** Called once from native code, on any thread. */
    public static void start() {
        MAIN.post(new Runnable() {
            @Override public void run() { startOnMain(); }
        });
    }

    private static void startOnMain() {
        Application app = currentApplication();
        ClassLoader cl = Thread.currentThread().getContextClassLoader();
        if (cl == null && app != null) cl = app.getClassLoader();
        Activity activity = cl != null ? unityActivity(cl) : null;
        if (app == null && activity != null) app = activity.getApplication();

        if (app != null && !callbacksRegistered) {
            app.registerActivityLifecycleCallbacks(new Lifecycle());
            callbacksRegistered = true;
        }
        if (activity != null) attach(activity);
        else if (app != null)
            log("no current activity yet, waiting for onActivityResumed (switch away from the game and back once)");
        else log("neither UnityPlayer.currentActivity nor the Application is reachable, menu not shown");
    }

    private static Application currentApplication() {
        try {
            Object o = Class.forName("android.app.ActivityThread").getMethod("currentApplication").invoke(null);
            return o instanceof Application ? (Application) o : null;
        } catch (Throwable t) {
            log("ActivityThread.currentApplication unavailable: " + t);
            return null;
        }
    }

    private static Activity unityActivity(ClassLoader cl) {
        try {
            Class<?> c = Class.forName("com.unity3d.player.UnityPlayer", false, cl);
            Object o = c.getField("currentActivity").get(null);
            return o instanceof Activity ? (Activity) o : null;
        } catch (Throwable t) {
            log("UnityPlayer.currentActivity unavailable: " + t);
            return null;
        }
    }

    private static void attach(Activity a) {
        if (a == null || a.isFinishing()) return;
        if (attachedTo.get() == a && root != null && root.getParent() != null) {
            forceRelayout("resume");
            return;
        }
        detach();
        try {
            root = build(a);
            // Full-screen container that never consumes touches itself: taps that miss the FAB and
            // the panel fall through to Unity's view underneath.
            a.addContentView(root, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            attachedTo = new WeakReference<Activity>(a);
            generation++;
            geometryLogs = 0;
            log("attached to " + a.getClass().getName());
            scheduleRelayouts();
            schedulePoll();
        } catch (Throwable t) {
            detach();
            log("attach failed: " + t);
        }
    }

    private static void detach() {
        if (root != null) {
            ViewParent p = root.getParent();
            if (p instanceof ViewGroup) ((ViewGroup) p).removeView(root);
        }
        root = null;
        fab = null;
        panel = null;
        statusPill = null;
        speedRow = lightRow = noclipRow = killRow = zoomRow = revealRow = ghostsRow = null;
        panelOpen = false;
        lastStatus = -1;
        generation++;
        attachedTo = new WeakReference<Activity>(null);
    }

    private static final class Lifecycle implements Application.ActivityLifecycleCallbacks {
        @Override public void onActivityResumed(Activity a) { attach(a); }
        @Override public void onActivityDestroyed(Activity a) { if (attachedTo.get() == a) detach(); }
        @Override public void onActivityCreated(Activity a, Bundle b) {}
        @Override public void onActivityStarted(Activity a) {}
        @Override public void onActivityPaused(Activity a) {}
        @Override public void onActivityStopped(Activity a) {}
        @Override public void onActivitySaveInstanceState(Activity a, Bundle b) {}
    }

    // ---------------------------------------------------------------- window geometry

    private static void scheduleRelayouts() {
        final int gen = generation;
        for (final long delay : RELAYOUT_DELAYS_MS) {
            MAIN.postDelayed(new Runnable() {
                @Override public void run() { if (gen == generation) forceRelayout("attach +" + delay + " ms"); }
            }, delay);
        }
    }

    /**
     * Re-applies the activity window's own attributes. ViewRootImpl then calls relayoutWindow() on
     * the next traversal, the same round trip the IME resize triggers, which is what re-synced the
     * drawn surface with the layout/input frame in the test report.
     */
    private static void forceRelayout(final String why) {
        final Activity a = attachedTo.get();
        if (a == null || a.isFinishing() || root == null) return;
        final long now = SystemClock.uptimeMillis();
        if (now - lastRelayoutAt < RELAYOUT_MIN_GAP_MS) return;
        lastRelayoutAt = now;
        final Window w = a.getWindow();
        if (w == null) return;
        try {
            w.setAttributes(w.getAttributes());
        } catch (Throwable t) {
            log("relayout (" + why + ") failed: " + t);
            return;
        }
        root.requestLayout();
        final int gen = generation;
        MAIN.postDelayed(new Runnable() {
            @Override public void run() {
                if (gen != generation) return;
                placeFab();
                if (panelOpen) placePanel();
                logGeometry("relayout " + why);
            }
        }, 150);
    }

    /** One line with every frame the menu depends on, so a bad device can be diffed before/after IME. */
    private static void logGeometry(String why) {
        if (root == null || geometryLogs >= MAX_GEOMETRY_LOGS) return;
        geometryLogs++;
        try {
            final Activity a = attachedTo.get();
            final int[] onScreen = new int[2], inWindow = new int[2];
            root.getLocationOnScreen(onScreen);
            root.getLocationInWindow(inWindow);
            final Rect visible = new Rect();
            root.getWindowVisibleDisplayFrame(visible);
            final DisplayMetrics dm = root.getResources().getDisplayMetrics();
            final Configuration cfg = root.getResources().getConfiguration();
            final Point real = new Point();
            View decor = null;
            if (a != null) {
                a.getWindowManager().getDefaultDisplay().getRealSize(real);
                if (a.getWindow() != null) decor = a.getWindow().getDecorView();
            }
            log(String.format(Locale.US,
                    "geometry [%s]: root %dx%d screen(%d,%d) window(%d,%d) decor %dx%d visibleFrame %s"
                            + " metrics %dx%d density %.3f real %dx%d cfg %dx%d dp fab(%.0f,%.0f %dx%d)",
                    why, root.getWidth(), root.getHeight(), onScreen[0], onScreen[1], inWindow[0], inWindow[1],
                    decor != null ? decor.getWidth() : -1, decor != null ? decor.getHeight() : -1,
                    visible.toShortString(), dm.widthPixels, dm.heightPixels, dm.density, real.x, real.y,
                    cfg.screenWidthDp, cfg.screenHeightDp, fab != null ? fab.getX() : -1f,
                    fab != null ? fab.getY() : -1f, fab != null ? fab.getWidth() : -1,
                    fab != null ? fab.getHeight() : -1));
        } catch (Throwable t) {
            log("geometry [" + why + "] unavailable: " + t);
        }
    }

    /** Overlay root: re-syncs the window when the activity configuration changes (fold, rotation). */
    private static final class Overlay extends FrameLayout {
        Overlay(Context c) { super(c); }

        @Override protected void onConfigurationChanged(Configuration cfg) {
            super.onConfigurationChanged(cfg);
            MAIN.post(new Runnable() {
                @Override public void run() { forceRelayout("configuration change"); }
            });
        }
    }

    // ---------------------------------------------------------------- layout

    private static FrameLayout build(Context c) {
        final float dp = c.getResources().getDisplayMetrics().density;
        float[] cfg = nativeConfig();
        if (cfg == null || cfg.length < CFG_LEN) cfg = new float[] {0f, 1.5f, 0f, 3f, 0f, 0f, 0f, 1.5f, 0f, 0f};

        final FrameLayout overlay = new Overlay(c);
        overlay.setClipChildren(false);

        fab = buildFab(c, dp);
        overlay.addView(fab, new FrameLayout.LayoutParams(px(54, dp), px(54, dp)));

        panel = buildPanel(c, dp, cfg);
        overlay.addView(panel, new FrameLayout.LayoutParams(px(300, dp), ViewGroup.LayoutParams.WRAP_CONTENT));
        panel.setVisibility(View.INVISIBLE);  // laid out (so it has a size) but not touchable

        overlay.addOnLayoutChangeListener(new View.OnLayoutChangeListener() {
            @Override public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
                if (r - l != or - ol || b - t != ob - ot) {
                    placeFab();
                    if (panel != null) panel.requestLayout();  // its scroll cap follows the root height
                    if (panelOpen) placePanel();
                    logGeometry("root resized");
                }
            }
        });
        refreshStatus();
        return overlay;
    }

    /** Event position in the coordinate space of the view's parent (the overlay root). */
    private static void inParent(View v, MotionEvent e, float[] out) {
        out[0] = e.getX();
        out[1] = e.getY();
        // getMatrix() holds translation, scale and pivot, so this also stays right during the press animation.
        v.getMatrix().mapPoints(out);
        out[0] += v.getLeft();
        out[1] += v.getTop();
    }

    private static TextView buildFab(Context c, final float dp) {
        final TextView v = new TextView(c);
        v.setText("AU");
        v.setTextColor(Color.WHITE);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        v.setTypeface(Typeface.DEFAULT_BOLD);
        v.setLetterSpacing(0.08f);
        v.setGravity(Gravity.CENTER);
        GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {ACCENT, ACCENT_2});
        bg.setShape(GradientDrawable.OVAL);
        bg.setStroke(px(2, dp), 0x40FFFFFF);
        v.setBackground(bg);
        v.setElevation(10 * dp);
        v.setAlpha(0.9f);

        final int slop = ViewConfiguration.get(c).getScaledTouchSlop();
        v.setOnTouchListener(new View.OnTouchListener() {
            // Drag is tracked in the overlay's own coordinates, not getRawX/Y: on a size-compat window
            // raw coordinates are display pixels while setX/setY are app pixels.
            private final float[] pt = new float[2];
            private float downX, downY, startX, startY;
            private boolean dragging;

            @Override public boolean onTouch(View view, MotionEvent e) {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        inParent(view, e, pt);
                        downX = pt[0];
                        downY = pt[1];
                        startX = view.getX();
                        startY = view.getY();
                        dragging = false;
                        view.animate().scaleX(0.9f).scaleY(0.9f).alpha(1f).setDuration(90).start();
                        return true;
                    case MotionEvent.ACTION_MOVE: {
                        inParent(view, e, pt);
                        float dx = pt[0] - downX, dy = pt[1] - downY;
                        if (!dragging && Math.hypot(dx, dy) > slop) {
                            dragging = true;
                            if (panelOpen) closePanel();
                        }
                        if (dragging) {
                            view.setX(clamp(startX + dx, 0, freeWidth()));
                            view.setY(clamp(startY + dy, 0, freeHeight()));
                        }
                        return true;
                    }
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        view.animate().scaleX(1f).scaleY(1f).alpha(0.9f).setDuration(120).start();
                        if (dragging) snapFab(dp);
                        else if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                            view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                            if (panelOpen) closePanel();
                            else openPanel();
                        }
                        return true;
                    default:
                        return false;
                }
            }
        });
        return v;
    }

    private static LinearLayout buildPanel(Context c, float dp, float[] cfg) {
        LinearLayout p = new LinearLayout(c);
        p.setOrientation(LinearLayout.VERTICAL);
        p.setBackground(rounded(BG, 18 * dp, STROKE, px(1, dp)));
        p.setElevation(14 * dp);
        p.setPadding(px(14, dp), px(12, dp), px(14, dp), px(12, dp));
        p.setClickable(true);  // touches inside the panel never reach the game

        // Header: accent dot, title + subtitle, status pill, close.
        LinearLayout header = new LinearLayout(c);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        View dot = new View(c);
        GradientDrawable dotBg = new GradientDrawable(GradientDrawable.Orientation.TL_BR, new int[] {ACCENT, ACCENT_2});
        dotBg.setShape(GradientDrawable.OVAL);
        dot.setBackground(dotBg);
        LinearLayout.LayoutParams dlp = new LinearLayout.LayoutParams(px(10, dp), px(10, dp));
        dlp.rightMargin = px(10, dp);
        header.addView(dot, dlp);

        LinearLayout titles = new LinearLayout(c);
        titles.setOrientation(LinearLayout.VERTICAL);
        titles.addView(text(c, "Among Us", 16, TEXT, Typeface.DEFAULT_BOLD));
        titles.addView(text(c, "Zygisk menu · " + VERSION, 11, MUTED, Typeface.DEFAULT));
        header.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        statusPill = text(c, "LOBBY", 10, MUTED, Typeface.DEFAULT_BOLD);
        statusPill.setLetterSpacing(0.08f);
        statusPill.setPadding(px(8, dp), px(3, dp), px(8, dp), px(3, dp));
        header.addView(statusPill);

        TextView close = text(c, "✕", 16, MUTED, Typeface.DEFAULT);
        close.setPadding(px(12, dp), px(4, dp), px(2, dp), px(4, dp));
        close.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) { closePanel(); }
        });
        header.addView(close);
        p.addView(header);

        MaxHeightScroll scroll = new MaxHeightScroll(c);
        LinearLayout content = new LinearLayout(c);
        content.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(content);
        LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        slp.topMargin = px(6, dp);
        p.addView(scroll, slp);

        section(c, dp, content, "MOVEMENT");
        speedRow = new FeatureRow(c, dp, content, "Speed", "Walk speed multiplier", SPEED,
                cfg[0] != 0f, cfg[1], SPEED_MIN, SPEED_MAX, true);
        noclipRow = new FeatureRow(c, dp, content, "Noclip", "Walk through walls", NOCLIP,
                cfg[4] != 0f, 0f, 0f, 1f, false);
        section(c, dp, content, "VISION");
        lightRow = new FeatureRow(c, dp, content, "Vision radius", "Light radius multiplier", LIGHT,
                cfg[2] != 0f, cfg[3], LIGHT_MIN, LIGHT_MAX, true);
        zoomRow = new FeatureRow(c, dp, content, "Zoom out", "Camera size multiplier", ZOOM,
                cfg[6] != 0f, cfg[7], ZOOM_MIN, ZOOM_MAX, true);
        section(c, dp, content, "INFO");
        revealRow = new FeatureRow(c, dp, content, "Reveal impostors", "Red names, local view only", REVEAL,
                cfg[8] != 0f, 0f, 0f, 1f, false);
        ghostsRow = new FeatureRow(c, dp, content, "See ghosts", "Show dead players while alive", GHOSTS,
                cfg[9] != 0f, 0f, 0f, 1f, false);
        section(c, dp, content, "IMPOSTOR");
        killRow = new FeatureRow(c, dp, content, "No kill cooldown", "Local timer only, the host may reject", KILL,
                cfg[5] != 0f, 0f, 0f, 1f, false);
        section(c, dp, content, "TOOLS");
        content.addView(dumpButton(c, dp));
        return p;
    }

    private static TextView dumpButton(Context c, float dp) {
        final TextView b = text(c, "Dump classes to au_dump.txt", 13, TEXT, Typeface.DEFAULT_BOLD);
        b.setGravity(Gravity.CENTER);
        b.setPadding(px(12, dp), px(10, dp), px(12, dp), px(10, dp));
        b.setBackground(rounded(0x00000000, 12 * dp, 0x33FFFFFF, px(1, dp)));
        b.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                nativeDump();
                v.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                b.setText("Dump requested ✓");
                b.setTextColor(OK);
                MAIN.postDelayed(new Runnable() {
                    @Override public void run() {
                        b.setText("Dump classes to au_dump.txt");
                        b.setTextColor(TEXT);
                    }
                }, 2000);
            }
        });
        return b;
    }

    private static void section(Context c, float dp, LinearLayout parent, String title) {
        TextView t = text(c, title, 10, MUTED, Typeface.DEFAULT_BOLD);
        t.setLetterSpacing(0.12f);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = px(10, dp);
        lp.bottomMargin = px(6, dp);
        lp.leftMargin = px(2, dp);
        parent.addView(t, lp);
    }

    // ---------------------------------------------------------------- placement and animation

    private static float freeWidth() { return root == null || fab == null ? 0 : Math.max(0, root.getWidth() - fab.getWidth()); }
    private static float freeHeight() { return root == null || fab == null ? 0 : Math.max(0, root.getHeight() - fab.getHeight()); }

    private static void placeFab() {
        if (fab == null) return;
        fab.setX(clamp(fabFx, 0f, 1f) * freeWidth());
        fab.setY(clamp(fabFy, 0f, 1f) * freeHeight());
    }

    private static void snapFab(float dp) {
        if (fab == null || root == null) return;
        final float margin = 10 * dp;
        final float fw = freeWidth(), fh = freeHeight();
        final float targetX = fab.getX() + fab.getWidth() / 2f < root.getWidth() / 2f ? margin : Math.max(margin, fw - margin);
        final float y = clamp(fab.getY(), margin, Math.max(margin, fh - margin));
        fabFx = fw > 0 ? targetX / fw : 0f;
        fabFy = fh > 0 ? y / fh : 0f;
        fab.animate().x(targetX).y(y).setDuration(220).setInterpolator(new DecelerateInterpolator()).start();
    }

    private static void placePanel() {
        if (panel == null || fab == null || root == null) return;
        final int pw = panel.getWidth(), ph = panel.getHeight();
        if (pw == 0 || ph == 0) {
            panel.post(new Runnable() { @Override public void run() { placePanel(); } });
            return;
        }
        final float dp = panel.getResources().getDisplayMetrics().density;
        final float gap = 10 * dp, margin = 8 * dp;
        final boolean fabOnLeft = fab.getX() + fab.getWidth() / 2f < root.getWidth() / 2f;
        float x = fabOnLeft ? fab.getX() + fab.getWidth() + gap : fab.getX() - pw - gap;
        x = clamp(x, margin, Math.max(margin, root.getWidth() - pw - margin));
        float y = clamp(fab.getY(), margin, Math.max(margin, root.getHeight() - ph - margin));
        panel.setX(x);
        panel.setY(y);
        panel.setPivotX(fabOnLeft ? 0f : pw);
        panel.setPivotY(Math.max(0f, Math.min(ph, fab.getY() + fab.getHeight() / 2f - y)));
    }

    private static void openPanel() {
        if (panel == null) return;
        panelOpen = true;
        placePanel();
        refreshStatus();
        panel.animate().cancel();
        panel.setAlpha(0f);
        panel.setScaleX(0.9f);
        panel.setScaleY(0.9f);
        panel.setVisibility(View.VISIBLE);
        panel.animate().alpha(1f).scaleX(1f).scaleY(1f).setDuration(180)
                .setInterpolator(new DecelerateInterpolator()).start();
    }

    private static void closePanel() {
        if (panel == null) return;
        panelOpen = false;
        final View p = panel;
        p.animate().cancel();
        p.animate().alpha(0f).scaleX(0.9f).scaleY(0.9f).setDuration(140)
                .withEndAction(new Runnable() {
                    @Override public void run() { if (!panelOpen) p.setVisibility(View.INVISIBLE); }
                }).start();
    }

    // ---------------------------------------------------------------- status

    private static void refreshStatus() {
        if (statusPill == null) return;
        final int st = nativeStatus();
        if (st == lastStatus) return;
        lastStatus = st;
        if (speedRow != null) speedRow.setAvailable((st & ST_SPEED) != 0);
        if (lightRow != null) lightRow.setAvailable((st & ST_LIGHT) != 0);
        if (noclipRow != null) noclipRow.setAvailable((st & ST_NOCLIP) != 0);
        if (killRow != null) killRow.setAvailable((st & ST_KILL) != 0);
        if (zoomRow != null) zoomRow.setAvailable((st & ST_ZOOM) != 0);
        if (revealRow != null) revealRow.setAvailable((st & ST_REVEAL) != 0);
        if (ghostsRow != null) ghostsRow.setAvailable((st & ST_GHOSTS) != 0);
        final boolean inGame = (st & ST_LOCAL) != 0;
        final float dp = statusPill.getResources().getDisplayMetrics().density;
        statusPill.setText(inGame ? "IN GAME" : "LOBBY");
        statusPill.setTextColor(inGame ? OK : MUTED);
        statusPill.setBackground(rounded(inGame ? 0x263DD68C : 0x1AFFFFFF, 10 * dp, 0, 0));
    }

    private static void schedulePoll() {
        if (polling) return;
        polling = true;
        MAIN.postDelayed(new Runnable() {
            @Override public void run() {
                if (root == null) {
                    polling = false;
                    return;
                }
                refreshStatus();
                MAIN.postDelayed(this, STATUS_PERIOD_MS);
            }
        }, STATUS_PERIOD_MS);
    }

    // ---------------------------------------------------------------- helpers

    private static TextView text(Context c, String s, float sp, int color, Typeface tf) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(tf);
        t.setIncludeFontPadding(false);
        return t;
    }

    private static GradientDrawable rounded(int color, float radius, int strokeColor, int strokeWidth) {
        GradientDrawable g = new GradientDrawable();
        g.setColor(color);
        g.setCornerRadius(radius);
        if (strokeWidth > 0) g.setStroke(strokeWidth, strokeColor);
        return g;
    }

    private static int px(float value, float dp) { return Math.round(value * dp); }

    private static float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

    private static int blend(int from, int to, float t) {
        t = clamp(t, 0f, 1f);
        int a = Math.round(Color.alpha(from) + (Color.alpha(to) - Color.alpha(from)) * t);
        int r = Math.round(Color.red(from) + (Color.red(to) - Color.red(from)) * t);
        int g = Math.round(Color.green(from) + (Color.green(to) - Color.green(from)) * t);
        int b = Math.round(Color.blue(from) + (Color.blue(to) - Color.blue(from)) * t);
        return Color.argb(a, r, g, b);
    }

    private static void log(String msg) {
        Log.i(TAG, "java menu: " + msg);
        try {
            nativeLog(msg);
        } catch (Throwable ignored) {
            // natives not registered yet; logcat already has the line
        }
    }

    // ---------------------------------------------------------------- widgets

    /** One card: title, subtitle, optional value chip and slider, and a toggle. */
    private static final class FeatureRow {
        final int id;
        final String subtitleText;
        final LinearLayout card;
        final TextView subtitle;
        final TextView chip;
        final Toggle toggle;
        final Slider slider;

        FeatureRow(Context c, float dp, LinearLayout parent, String title, String sub, int id, boolean on,
                   float value, float min, float max, boolean withSlider) {
            this.id = id;
            this.subtitleText = sub;
            card = new LinearLayout(c);
            card.setOrientation(LinearLayout.VERTICAL);
            card.setBackground(rounded(SURFACE, 14 * dp, 0, 0));
            card.setPadding(px(12, dp), px(10, dp), px(12, dp), px(10, dp));

            LinearLayout top = new LinearLayout(c);
            top.setOrientation(LinearLayout.HORIZONTAL);
            top.setGravity(Gravity.CENTER_VERTICAL);
            LinearLayout col = new LinearLayout(c);
            col.setOrientation(LinearLayout.VERTICAL);
            col.addView(text(c, title, 14, TEXT, Typeface.DEFAULT_BOLD));
            subtitle = text(c, sub, 11, MUTED, Typeface.DEFAULT);
            LinearLayout.LayoutParams sublp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            sublp.topMargin = px(3, dp);
            col.addView(subtitle, sublp);
            top.addView(col, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

            if (withSlider) {
                chip = text(c, "", 12, ACCENT_2, Typeface.DEFAULT_BOLD);
                chip.setPadding(px(8, dp), px(3, dp), px(8, dp), px(3, dp));
                chip.setBackground(rounded(0x26FF8A3D, 8 * dp, 0, 0));
                LinearLayout.LayoutParams clp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT);
                clp.rightMargin = px(10, dp);
                top.addView(chip, clp);
            } else {
                chip = null;
            }
            toggle = new Toggle(c, dp, on);
            top.addView(toggle);
            card.addView(top);

            if (withSlider) {
                slider = new Slider(c, dp, min, max, value, on);
                LinearLayout.LayoutParams slp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT);
                slp.topMargin = px(6, dp);
                card.addView(slider, slp);
                slider.listener = new Slider.Listener() {
                    @Override public void onValue(float v, boolean done) {
                        showValue(v);
                        if (done) push();
                    }
                };
                showValue(slider.value);
            } else {
                slider = null;
            }

            toggle.listener = new Toggle.Listener() {
                @Override public void onToggle(boolean checked) {
                    toggle.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                    if (slider != null) slider.setActive(checked);
                    push();
                }
            };
            card.setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { toggle.toggle(); }
            });

            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.bottomMargin = px(8, dp);
            parent.addView(card, lp);
        }

        void showValue(float v) { if (chip != null) chip.setText(String.format(Locale.US, "×%.2f", v)); }

        void push() { nativeSet(id, toggle.isChecked(), slider != null ? slider.value : 0f); }

        void setAvailable(boolean ok) {
            card.setAlpha(ok ? 1f : 0.4f);
            card.setEnabled(ok);
            toggle.setEnabled(ok);
            if (slider != null) slider.setEnabled(ok);
            subtitle.setText(ok ? subtitleText : "Unavailable on this build");
        }
    }

    /** Pill switch drawn on a canvas, independent of the host app's theme. */
    private static final class Toggle extends View {
        interface Listener { void onToggle(boolean checked); }

        private final Paint track = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint thumb = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final float dp;
        private boolean checked;
        private float pos;  // 0 = off, 1 = on
        private ValueAnimator anim;
        Listener listener;

        Toggle(Context c, float dp, boolean on) {
            super(c);
            this.dp = dp;
            checked = on;
            pos = on ? 1f : 0f;
            thumb.setColor(Color.WHITE);
            setClickable(true);
            setOnClickListener(new View.OnClickListener() {
                @Override public void onClick(View v) { toggle(); }
            });
        }

        boolean isChecked() { return checked; }

        void toggle() {
            if (!isEnabled()) return;
            checked = !checked;
            animateTo(checked ? 1f : 0f);
            if (listener != null) listener.onToggle(checked);
        }

        private void animateTo(float target) {
            if (anim != null) anim.cancel();
            anim = ValueAnimator.ofFloat(pos, target);
            anim.setDuration(160);
            anim.setInterpolator(new DecelerateInterpolator());
            anim.addUpdateListener(new ValueAnimator.AnimatorUpdateListener() {
                @Override public void onAnimationUpdate(ValueAnimator a) {
                    pos = (Float) a.getAnimatedValue();
                    invalidate();
                }
            });
            anim.start();
        }

        @Override protected void onMeasure(int w, int h) { setMeasuredDimension(px(44, dp), px(26, dp)); }

        @Override protected void onDraw(Canvas cv) {
            final float w = getWidth(), h = getHeight(), r = h / 2f;
            track.setColor(blend(OFF, ACCENT, pos));
            rect.set(0, 0, w, h);
            cv.drawRoundRect(rect, r, r, track);
            final float cx = r + (w - 2 * r) * pos;
            cv.drawCircle(cx, r, r - 3 * dp, thumb);
        }
    }

    /** Horizontal slider with a fixed step, reporting live values and a final value on release. */
    private static final class Slider extends View {
        interface Listener { void onValue(float v, boolean done); }

        private final Paint base = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint knob = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF rect = new RectF();
        private final float dp, min, max;
        private boolean active;
        float value;
        Listener listener;

        Slider(Context c, float dp, float min, float max, float initial, boolean active) {
            super(c);
            this.dp = dp;
            this.min = min;
            this.max = max;
            this.active = active;
            this.value = snap(initial);
            base.setColor(OFF);
            knob.setColor(Color.WHITE);
            setClickable(true);
        }

        void setActive(boolean a) {
            active = a;
            invalidate();
        }

        private float snap(float v) {
            if (!(v >= min)) v = min;  // also catches NaN
            if (v > max) v = max;
            return clamp(Math.round(v / SLIDER_STEP) * SLIDER_STEP, min, max);
        }

        private float knobRadius() { return 8 * dp; }

        @Override protected void onMeasure(int wSpec, int hSpec) {
            int w = MeasureSpec.getMode(wSpec) == MeasureSpec.UNSPECIFIED ? px(220, dp) : MeasureSpec.getSize(wSpec);
            setMeasuredDimension(w, px(28, dp));
        }

        @Override protected void onDraw(Canvas cv) {
            final float r = knobRadius(), cy = getHeight() / 2f;
            final float left = r, right = getWidth() - r;
            final float t = (value - min) / (max - min);
            final float x = left + (right - left) * t;
            final float th = 2 * dp;
            rect.set(left, cy - th, right, cy + th);
            cv.drawRoundRect(rect, th, th, base);
            fill.setColor(active ? ACCENT : MUTED);
            rect.set(left, cy - th, x, cy + th);
            cv.drawRoundRect(rect, th, th, fill);
            cv.drawCircle(x, cy, r, knob);
            cv.drawCircle(x, cy, r * 0.45f, fill);
        }

        @Override public boolean onTouchEvent(MotionEvent e) {
            if (!isEnabled()) return false;
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    if (getParent() != null) getParent().requestDisallowInterceptTouchEvent(true);
                    update(e.getX(), false);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    update(e.getX(), false);
                    return true;
                case MotionEvent.ACTION_UP:
                case MotionEvent.ACTION_CANCEL:
                    update(e.getX(), true);
                    return true;
                default:
                    return false;
            }
        }

        private void update(float x, boolean done) {
            final float r = knobRadius();
            final float span = Math.max(1f, getWidth() - 2 * r);
            final float t = clamp((x - r) / span, 0f, 1f);
            final float v = snap(min + (max - min) * t);
            final boolean changed = v != value;
            value = v;
            if (changed) invalidate();
            if (listener != null && (changed || done)) listener.onValue(value, done);
        }
    }

    /**
     * ScrollView capped at ~62% of the overlay height, so the panel fits in landscape. The overlay
     * height is the app's real area; DisplayMetrics can describe the display on a size-compat window.
     */
    private static final class MaxHeightScroll extends ScrollView {
        MaxHeightScroll(Context c) {
            super(c);
            setVerticalScrollBarEnabled(false);
            setOverScrollMode(View.OVER_SCROLL_NEVER);
        }

        @Override protected void onMeasure(int wSpec, int hSpec) {
            final int avail = root != null && root.getHeight() > 0 ? root.getHeight()
                    : getResources().getDisplayMetrics().heightPixels;
            int max = Math.round(avail * 0.62f);
            super.onMeasure(wSpec, MeasureSpec.makeMeasureSpec(max, MeasureSpec.AT_MOST));
        }
    }
}
