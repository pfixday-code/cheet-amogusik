package au.menu;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
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
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.lang.ref.WeakReference;
import java.util.Locale;

/**
 * Floating mod menu drawn with plain Android views (custom toggle and slider, no theme or
 * resources needed), so it looks the same on every Unity build and works on GLES and Vulkan.
 *
 * The FAB and the panel each live in their own sub-window of the game's activity window
 * (TYPE_APPLICATION_PANEL). Drawn inside the Unity window itself, their new frames were not
 * presented on a size-compat screen until an IME resize; a sub-window has its own surface and its
 * own traversals. If the sub-windows cannot be added, the views go into the activity content as
 * before (fallback, logged).
 */
public final class Menu {
    private static final String TAG = "AUMod";
    private static final String VERSION = "v2.7.0";

    // Feature ids, player action ids and status bits are shared with src/java_menu.cpp.
    private static final int SPEED = 0, LIGHT = 1, NOCLIP = 2, KILL = 3, ZOOM = 4, REVEAL = 5, GHOSTS = 6;
    private static final int WALL_VISION = 7, INVISIBLE = 8;
    private static final int ST_SPEED = 1, ST_NOCLIP = 2, ST_LIGHT = 4, ST_KILL = 8, ST_LOCAL = 16,
            ST_ZOOM = 32, ST_REVEAL = 64, ST_GHOSTS = 128, ST_WALL_VISION = 256, ST_INVISIBLE = 512,
            ST_PLAYERS = 1024, ST_TELEPORT = 2048, ST_BRING = 4096, ST_SPECTATE = 8192,
            ST_REPORT = 16384, ST_COMPLETE_TASKS = 32768;
    private static final int TELEPORT = 1, BRING = 2, SPECTATE = 3, RETURN_CAMERA = 4,
            REPORT_BODY = 5, COMPLETE_TASKS = 6;
    // nativeConfig(): speedOn, speedMul, lightOn, lightMul, noclip, killOn,
    // zoomOn, zoomMul, revealOn, ghostsOn, wallVisionOn, invisibleOn.
    private static final int CFG_LEN = 12;
    // Must match kSpeedMul* / kLightMul* / kZoomMul* in src/common.h.
    private static final float SPEED_MIN = 0.5f, SPEED_MAX = 4f, LIGHT_MIN = 0.5f, LIGHT_MAX = 8f;
    private static final float ZOOM_MIN = 1f, ZOOM_MAX = 4f;
    private static final float SLIDER_STEP = 0.05f;
    private static final long STATUS_PERIOD_MS = 1000;
    private static final int FAB_DP = 54, PANEL_DP = 300;
    // The activity window needs a token and a size before sub-windows can be attached to it.
    private static final int ATTACH_ATTEMPTS = 50;
    private static final long ATTACH_RETRY_MS = 100;
    // Diagnostics are bounded to keep the report useful on long sessions.
    private static final int MAX_GEOMETRY_LOGS = 40, MAX_TOUCH_LOGS = 60, MAX_MOVE_LOGS_PER_GESTURE = 3;
    // Below this raw distance the raw-to-app scale is not measured from a gesture.
    private static final float MIN_CALIBRATION_RAW_PX = 8f;

    // Palette.
    private static final int BG = 0xF2181A20, SURFACE = 0xFF23262F, STROKE = 0x1FFFFFFF;
    private static final int TEXT = 0xFFF2F3F5, MUTED = 0xFF8D93A0, OFF = 0xFF3A3D47;
    private static final int ACCENT = 0xFFE5484D, ACCENT_2 = 0xFFFF8A3D, OK = 0xFF3DD68C;

    private static native void nativeSet(int id, boolean on, float value);
    private static native int nativeStatus();
    private static native float[] nativeConfig();
    private static native String[] nativePlayers();
    private static native boolean nativeAction(int action, int targetId);
    private static native String nativeActionInfo();
    private static native void nativeDump();
    private static native void nativeLog(String msg);

    private static final Handler MAIN = new Handler(Looper.getMainLooper());

    private static WeakReference<Activity> attachedTo = new WeakReference<Activity>(null);
    private static boolean callbacksRegistered;
    private static boolean polling;

    // true: FAB and panel are sub-windows; false: both live in contentRoot inside the activity window.
    private static boolean windowMode;
    private static WindowManager wm;
    private static WindowManager.LayoutParams fabLp, panelLp;
    private static View parentView;          // decor view (window mode) or contentRoot (fallback)
    private static FrameLayout contentRoot;  // fallback only
    private static FrameLayout fabHost;      // receives the FAB touches, never transformed
    private static TextView fab;             // drawn button inside fabHost, carries the press animation
    private static LinearLayout panel;
    private static TextView statusPill;
    private static FeatureRow speedRow, lightRow, noclipRow, killRow, zoomRow, revealRow, ghostsRow;
    private static FeatureRow wallVisionRow, invisibleRow;
    private static TextView targetText, previousPlayer, nextPlayer, actionInfo;
    private static TextView teleportButton, bringButton, spectateButton, returnCameraButton;
    private static TextView reportButton, completeTasksButton;
    private static String[] playerRows = new String[0];
    private static int selectedPlayer;
    private static int lastActionSequence;
    private static boolean panelOpen;
    private static int lastStatus = -1;
    // Bumped on every attach and detach, so delayed work posted for an old menu does nothing.
    private static int generation;
    private static int geometryLogs, touchLogs;
    private static int fabDraws, panelDraws;
    // App pixels per raw pixel, measured from drags (0 = not measured yet) and the raw distance it came from.
    private static float rawScale;
    private static double rawScaleBasis;
    // addView() attaches on the next traversal, so isAttachedToWindow() cannot tell what to remove.
    private static boolean fabAdded, panelAdded;
    private static final View.OnLayoutChangeListener PARENT_RESIZE = new View.OnLayoutChangeListener() {
        @Override public void onLayoutChange(View v, int l, int t, int r, int b, int ol, int ot, int or, int ob) {
            if (r - l != or - ol || b - t != ob - ot) onParentResized("parent resized");
        }
    };
    // Applied positions in the parent's coordinates (app pixels); fractions survive re-attach.
    private static float fabX, fabY;
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
        if (attachedTo.get() == a && fabHost != null && fabHost.isAttachedToWindow()) return;
        detach();
        attachedTo = new WeakReference<Activity>(a);
        geometryLogs = 0;
        touchLogs = 0;
        tryAttach(a, generation, 0);
    }

    private static void tryAttach(final Activity a, final int gen, final int attempt) {
        if (gen != generation || a.isFinishing()) return;
        final View decor = a.getWindow() != null ? a.getWindow().getDecorView() : null;
        if (decor == null || decor.getWindowToken() == null || decor.getWidth() == 0 || decor.getHeight() == 0) {
            if (attempt < ATTACH_ATTEMPTS) {
                MAIN.postDelayed(new Runnable() {
                    @Override public void run() { tryAttach(a, gen, attempt + 1); }
                }, ATTACH_RETRY_MS);
            } else {
                log("activity window has no token/size after " + ATTACH_ATTEMPTS * ATTACH_RETRY_MS
                        + " ms, using the in-content fallback");
                attachInContent(a);
            }
            return;
        }
        try {
            attachAsWindows(a, decor);
        } catch (Throwable t) {
            log("sub-window attach failed: " + t + "; using the in-content fallback");
            removeMenuViews();
            attachInContent(a);
        }
    }

    private static void attachAsWindows(Activity a, View decor) {
        final float dp = a.getResources().getDisplayMetrics().density;
        buildViews(a, dp);
        // A WRAP_CONTENT window clips the shadow to a hard edge; the stroke already outlines the panel.
        panel.setElevation(0f);
        wm = a.getWindowManager();
        final IBinder token = decor.getWindowToken();
        fabLp = subWindowParams(token, px(FAB_DP, dp), px(FAB_DP, dp));
        panelLp = subWindowParams(token, px(PANEL_DP, dp), ViewGroup.LayoutParams.WRAP_CONTENT);
        parentView = decor;
        windowMode = true;
        fabX = clamp(fabFx, 0f, 1f) * freeWidth();
        fabY = clamp(fabFy, 0f, 1f) * freeHeight();
        fabLp.x = Math.round(fabX);
        fabLp.y = Math.round(fabY);
        panel.setVisibility(View.INVISIBLE);  // an invisible root view hides its window and its touch area
        wm.addView(fabHost, fabLp);
        fabAdded = true;
        wm.addView(panel, panelLp);
        panelAdded = true;
        decor.addOnLayoutChangeListener(PARENT_RESIZE);
        installDrawCounters();
        log("attached to " + a.getClass().getName() + " as sub-windows");
        logGeometry("attach");
        schedulePoll();
    }

    private static void attachInContent(Activity a) {
        try {
            final float dp = a.getResources().getDisplayMetrics().density;
            removeMenuViews();
            buildViews(a, dp);
            windowMode = false;
            contentRoot = new FrameLayout(a);
            contentRoot.setClipChildren(false);
            contentRoot.addView(fabHost, new FrameLayout.LayoutParams(px(FAB_DP, dp), px(FAB_DP, dp)));
            contentRoot.addView(panel, new FrameLayout.LayoutParams(px(PANEL_DP, dp), ViewGroup.LayoutParams.WRAP_CONTENT));
            panel.setVisibility(View.INVISIBLE);
            // Full-screen container that never consumes touches itself.
            a.addContentView(contentRoot, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
            parentView = contentRoot;
            contentRoot.addOnLayoutChangeListener(PARENT_RESIZE);
            installDrawCounters();
            log("attached to " + a.getClass().getName() + " in content (fallback)");
            schedulePoll();
        } catch (Throwable t) {
            detach();
            log("attach failed: " + t);
        }
    }

    private static WindowManager.LayoutParams subWindowParams(IBinder token, int w, int h) {
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams(w, h,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_HARDWARE_ACCELERATED,
                PixelFormat.TRANSLUCENT);
        lp.gravity = Gravity.TOP | Gravity.LEFT;
        lp.token = token;
        if (Build.VERSION.SDK_INT >= 28)
            lp.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        lp.setTitle("AUMenu");
        return lp;
    }

    /** Removes the FAB and panel from wherever they are; safe to call in any state. */
    private static void removeMenuViews() {
        if (parentView != null) parentView.removeOnLayoutChangeListener(PARENT_RESIZE);
        if (wm != null) {
            if (fabAdded && fabHost != null) removeWindow(fabHost);
            if (panelAdded && panel != null) removeWindow(panel);
        }
        fabAdded = panelAdded = false;
        if (contentRoot != null) {
            ViewParent p = contentRoot.getParent();
            if (p instanceof ViewGroup) ((ViewGroup) p).removeView(contentRoot);
        }
        wm = null;
        fabLp = panelLp = null;
        parentView = null;
        contentRoot = null;
    }

    private static void removeWindow(View v) {
        try {
            wm.removeViewImmediate(v);
        } catch (Throwable t) {
            // The parent window may already be gone (activity destroyed), which removes ours with it.
            log("removing a menu window: " + t);
        }
    }

    private static void detach() {
        removeMenuViews();
        fabHost = null;
        fab = null;
        panel = null;
        statusPill = null;
        speedRow = lightRow = noclipRow = killRow = zoomRow = revealRow = ghostsRow = null;
        wallVisionRow = invisibleRow = null;
        targetText = previousPlayer = nextPlayer = actionInfo = null;
        teleportButton = bringButton = spectateButton = returnCameraButton = null;
        reportButton = completeTasksButton = null;
        playerRows = new String[0];
        selectedPlayer = 0;
        lastActionSequence = 0;
        panelOpen = false;
        windowMode = false;
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

    private static void onParentResized(String why) {
        if (fabHost == null) return;
        placeFab();
        if (panel != null) panel.requestLayout();  // its scroll cap follows the parent height
        if (panelOpen) placePanel();
        logGeometry(why);
    }

    // ---------------------------------------------------------------- diagnostics

    private static void installDrawCounters() {
        fabDraws = panelDraws = 0;
        fabHost.getViewTreeObserver().addOnDrawListener(new ViewTreeObserver.OnDrawListener() {
            @Override public void onDraw() { fabDraws++; }
        });
        panel.getViewTreeObserver().addOnDrawListener(new ViewTreeObserver.OnDrawListener() {
            @Override public void onDraw() { panelDraws++; }
        });
    }

    /** One line with every frame the menu depends on, so a bad device can be diffed before/after IME. */
    private static void logGeometry(String why) {
        if (fabHost == null || parentView == null || geometryLogs >= MAX_GEOMETRY_LOGS) return;
        geometryLogs++;
        try {
            final Activity a = attachedTo.get();
            final int[] parentOnScreen = new int[2], fabOnScreen = new int[2];
            parentView.getLocationOnScreen(parentOnScreen);
            fabHost.getLocationOnScreen(fabOnScreen);
            final Rect visible = new Rect();
            parentView.getWindowVisibleDisplayFrame(visible);
            final DisplayMetrics dm = parentView.getResources().getDisplayMetrics();
            final Configuration cfg = parentView.getResources().getConfiguration();
            final Point real = new Point();
            if (a != null) a.getWindowManager().getDefaultDisplay().getRealSize(real);
            log(String.format(Locale.US,
                    "geometry [%s]: mode %s parent %dx%d screen(%d,%d) visibleFrame %s metrics %dx%d density %.3f"
                            + " real %dx%d cfg %dx%d dp fab(%.0f,%.0f %dx%d) fabScreen(%d,%d)",
                    why, windowMode ? "windows" : "content", parentView.getWidth(), parentView.getHeight(),
                    parentOnScreen[0], parentOnScreen[1], visible.toShortString(), dm.widthPixels, dm.heightPixels,
                    dm.density, real.x, real.y, cfg.screenWidthDp, cfg.screenHeightDp, fabX, fabY,
                    fabHost.getWidth(), fabHost.getHeight(), fabOnScreen[0], fabOnScreen[1]));
        } catch (Throwable t) {
            log("geometry [" + why + "] unavailable: " + t);
        }
    }

    /** What the panel really is at this moment: answers "alpha stuck" vs "no traversal" vs "no input". */
    private static void logPanelState(String why) {
        if (panel == null) return;
        final int[] onScreen = new int[2];
        panel.getLocationOnScreen(onScreen);
        log(String.format(Locale.US,
                "panel [%s]: mode %s open %b visibility %d alpha %.2f scale %.2f shown %b attached %b laidOut %b"
                        + " size %dx%d at(%.0f,%.0f) screen(%d,%d) windowVisibility %d draws fab %d panel %d",
                why, windowMode ? "windows" : "content", panelOpen, panel.getVisibility(), panel.getAlpha(),
                panel.getScaleX(), panel.isShown(), panel.isAttachedToWindow(), panel.isLaidOut(), panel.getWidth(),
                panel.getHeight(), windowMode && panelLp != null ? (float) panelLp.x : panel.getX(),
                windowMode && panelLp != null ? (float) panelLp.y : panel.getY(), onScreen[0], onScreen[1],
                panel.getWindowVisibility(), fabDraws, panelDraws));
    }

    private static void logTouch(String who, MotionEvent e) {
        if (touchLogs >= MAX_TOUCH_LOGS) return;
        touchLogs++;
        log(String.format(Locale.US, "touch %s down local(%.0f,%.0f) raw(%.0f,%.0f)", who, e.getX(), e.getY(),
                e.getRawX(), e.getRawY()));
    }

    // ---------------------------------------------------------------- layout

    private static void buildViews(Context c, float dp) {
        float[] cfg = nativeConfig();
        if (cfg == null || cfg.length < CFG_LEN)
            cfg = new float[] {0f, 1.5f, 0f, 3f, 0f, 0f, 0f, 1.5f, 0f, 0f, 0f, 0f};
        fabHost = new FabHost(c);
        fabHost.setClipChildren(false);
        fab = buildFab(c, dp);
        fabHost.addView(fab, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT));
        fabHost.setOnTouchListener(fabTouch(c, dp));
        panel = buildPanel(c, dp, cfg);
        refreshStatus();
    }

    /** FAB container: also re-places the menu when the activity configuration changes (fold, rotation). */
    private static final class FabHost extends FrameLayout {
        FabHost(Context c) { super(c); }

        @Override protected void onConfigurationChanged(Configuration cfg) {
            super.onConfigurationChanged(cfg);
            MAIN.post(new Runnable() {
                @Override public void run() { onParentResized("configuration change"); }
            });
        }
    }

    private static TextView buildFab(Context c, float dp) {
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
        v.setAlpha(0.9f);
        return v;
    }

    /**
     * Event position in the parent's coordinates. The host's window/view offset is read from the
     * framework at event time, so a window move that has not landed yet cannot make the drag jump,
     * and raw (display-pixel) coordinates are never mixed with app pixels.
     */
    private static void inParent(View host, MotionEvent e, int[] tmpHost, int[] tmpParent, float[] out) {
        host.getLocationOnScreen(tmpHost);
        parentView.getLocationOnScreen(tmpParent);
        out[0] = tmpHost[0] - tmpParent[0] + e.getX();
        out[1] = tmpHost[1] - tmpParent[1] + e.getY();
    }

    private static View.OnTouchListener fabTouch(Context c, final float dp) {
        final int slop = ViewConfiguration.get(c).getScaledTouchSlop();
        return new View.OnTouchListener() {
            private final int[] a = new int[2], b = new int[2];
            private final float[] pt = new float[2];
            private float downLX, downLY, downRX, downRY, downX, downY, startX, startY;
            private float scale;  // app pixels per raw pixel for this gesture
            private boolean dragging, useRaw;
            private int moves, moveLogs;

            @Override public boolean onTouch(View view, MotionEvent e) {
                if (parentView == null || fab == null) return false;
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        logTouch("fab", e);
                        downLX = e.getX();
                        downLY = e.getY();
                        downRX = e.getRawX();
                        downRY = e.getRawY();
                        inParent(view, e, a, b, pt);
                        downX = pt[0];
                        downY = pt[1];
                        startX = fabX;
                        startY = fabY;
                        dragging = false;
                        useRaw = false;
                        moves = 0;
                        moveLogs = 0;
                        fab.animate().scaleX(0.9f).scaleY(0.9f).alpha(1f).setDuration(90).start();
                        return true;
                    case MotionEvent.ACTION_MOVE: {
                        moves++;
                        if (moveLogs < MAX_MOVE_LOGS_PER_GESTURE) {
                            moveLogs++;
                            logMove(e);
                        }
                        if (!dragging) {
                            // The window has not moved yet, so local deltas are exact here.
                            final float dlx = e.getX() - downLX, dly = e.getY() - downLY;
                            if (Math.hypot(dlx, dly) <= slop) return true;
                            dragging = true;
                            if (panelOpen) closePanel();
                            startDrag(dlx, dly, e.getRawX() - downRX, e.getRawY() - downRY);
                        }
                        float dx, dy;
                        if (useRaw) {
                            // Raw coordinates are screen space: moving our own window cannot feed back into them.
                            dx = (e.getRawX() - downRX) * scale;
                            dy = (e.getRawY() - downRY) * scale;
                        } else {
                            inParent(view, e, a, b, pt);
                            dx = pt[0] - downX;
                            dy = pt[1] - downY;
                        }
                        applyFab(clamp(startX + dx, 0, freeWidth()), clamp(startY + dy, 0, freeHeight()));
                        return true;
                    }
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        fab.animate().scaleX(1f).scaleY(1f).alpha(0.9f).setDuration(120).start();
                        if (dragging) {
                            log(String.format(Locale.US,
                                    "drag end: %d moves, raw delta (%.0f,%.0f), FAB moved (%.0f,%.0f) before snap, path %s",
                                    moves, e.getRawX() - downRX, e.getRawY() - downRY, fabX - startX, fabY - startY,
                                    useRaw ? "raw" : "local"));
                            snapFab(dp);
                        } else if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                            view.performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
                            if (panelOpen) closePanel();
                            else openPanel();
                        }
                        return true;
                    default:
                        return false;
                }
            }

            /**
             * Measures the raw-to-app scale from the slop phase, where the window was still in place:
             * on a size-compat window raw is display pixels (here ~1/0.75 of app pixels). The best
             * measurement (largest raw distance) is kept across gestures.
             */
            private void startDrag(float dlx, float dly, float drx, float dry) {
                final double dl = Math.hypot(dlx, dly), dr = Math.hypot(drx, dry);
                final boolean sameDirection = dlx * drx + dly * dry > 0;
                if (dr >= MIN_CALIBRATION_RAW_PX && sameDirection) {
                    final float k = (float) (dl / dr);
                    if (k >= 0.25f && k <= 4f && dr > rawScaleBasis) {
                        rawScale = k;
                        rawScaleBasis = dr;
                    }
                }
                useRaw = rawScale > 0f && dr >= MIN_CALIBRATION_RAW_PX && sameDirection;
                scale = rawScale;
                log(String.format(Locale.US, "drag start: local delta (%.0f,%.0f) raw delta (%.0f,%.0f) scale %.3f path %s",
                        dlx, dly, drx, dry, rawScale, useRaw ? "raw" : "local (raw unusable)"));
            }
        };
    }

    private static void logMove(MotionEvent e) {
        if (touchLogs >= MAX_TOUCH_LOGS) return;
        touchLogs++;
        log(String.format(Locale.US, "touch fab move local(%.0f,%.0f) raw(%.0f,%.0f)", e.getX(), e.getY(),
                e.getRawX(), e.getRawY()));
    }

    private static LinearLayout buildPanel(Context c, float dp, float[] cfg) {
        LinearLayout p = new PanelLayout(c);
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
        zoomRow = new FeatureRow(c, dp, content, "Zoom out", "World camera only; restores in meetings", ZOOM,
                cfg[6] != 0f, cfg[7], ZOOM_MIN, ZOOM_MAX, true);
        wallVisionRow = new FeatureRow(c, dp, content, "Wall vision", "Hide wall shadows without Noclip", WALL_VISION,
                cfg[10] != 0f, 0f, 0f, 1f, false);
        section(c, dp, content, "INFO");
        revealRow = new FeatureRow(c, dp, content, "Reveal impostors", "Red names, local view only", REVEAL,
                cfg[8] != 0f, 0f, 0f, 1f, false);
        ghostsRow = new FeatureRow(c, dp, content, "See ghosts", "Show dead players while alive", GHOSTS,
                cfg[9] != 0f, 0f, 0f, 1f, false);
        invisibleRow = new FeatureRow(c, dp, content, "Local invisibility", "Your screen only; others still see you", INVISIBLE,
                cfg[11] != 0f, 0f, 0f, 1f, false);
        section(c, dp, content, "IMPOSTOR");
        killRow = new FeatureRow(c, dp, content, "No kill cooldown", "Local timer only, the host may reject", KILL,
                cfg[5] != 0f, 0f, 0f, 1f, false);
        section(c, dp, content, "PLAYERS");
        buildPlayerActions(c, dp, content);
        section(c, dp, content, "TOOLS");
        content.addView(dumpButton(c, dp));
        return p;
    }

    private static void buildPlayerActions(Context c, float dp, LinearLayout content) {
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(rounded(SURFACE, 14 * dp, 0, 0));
        card.setPadding(px(12, dp), px(10, dp), px(12, dp), px(10, dp));

        LinearLayout chooser = new LinearLayout(c);
        chooser.setOrientation(LinearLayout.HORIZONTAL);
        chooser.setGravity(Gravity.CENTER_VERTICAL);
        previousPlayer = chooserButton(c, dp, "‹", -1);
        chooser.addView(previousPlayer, new LinearLayout.LayoutParams(px(42, dp), ViewGroup.LayoutParams.WRAP_CONTENT));
        targetText = text(c, "Target: enter a match", 12, TEXT, Typeface.DEFAULT_BOLD);
        targetText.setGravity(Gravity.CENTER);
        targetText.setSingleLine(true);
        targetText.setPadding(px(5, dp), 0, px(5, dp), 0);
        chooser.addView(targetText, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        nextPlayer = chooserButton(c, dp, "›", 1);
        chooser.addView(nextPlayer, new LinearLayout.LayoutParams(px(42, dp), ViewGroup.LayoutParams.WRAP_CONTENT));
        card.addView(chooser);

        actionInfo = text(c, "Actions run on the next Unity tick", 10, MUTED, Typeface.DEFAULT);
        LinearLayout.LayoutParams infoLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        infoLp.topMargin = px(8, dp);
        card.addView(actionInfo, infoLp);
        LinearLayout.LayoutParams cardLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        cardLp.bottomMargin = px(8, dp);
        content.addView(card, cardLp);

        teleportButton = actionButton(c, dp, "Teleport to player", TELEPORT);
        bringButton = actionButton(c, dp, "Bring player (host / Freeplay)", BRING);
        spectateButton = actionButton(c, dp, "Spectate player (local camera)", SPECTATE);
        returnCameraButton = actionButton(c, dp, "Return camera and movement", RETURN_CAMERA);
        reportButton = actionButton(c, dp, "Report selected body", REPORT_BODY);
        completeTasksButton = actionButton(c, dp, "Complete all my tasks", COMPLETE_TASKS);
        content.addView(teleportButton);
        content.addView(bringButton);
        content.addView(spectateButton);
        content.addView(returnCameraButton);
        content.addView(reportButton);
        content.addView(completeTasksButton);
    }

    private static TextView chooserButton(Context c, float dp, String label, final int direction) {
        TextView b = text(c, label, 20, TEXT, Typeface.DEFAULT_BOLD);
        b.setGravity(Gravity.CENTER);
        b.setPadding(0, px(8, dp), 0, px(8, dp));
        b.setBackground(rounded(OFF, 9 * dp, 0, 0));
        b.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                if (playerRows.length == 0) return;
                selectedPlayer = (selectedPlayer + direction + playerRows.length) % playerRows.length;
                updateTargetText();
            }
        });
        return b;
    }

    private static TextView actionButton(Context c, float dp, String label, final int action) {
        final TextView b = text(c, label, 12, TEXT, Typeface.DEFAULT_BOLD);
        b.setGravity(Gravity.CENTER);
        b.setPadding(px(12, dp), px(11, dp), px(12, dp), px(11, dp));
        b.setBackground(rounded(SURFACE, 12 * dp, STROKE, px(1, dp)));
        b.setOnClickListener(new View.OnClickListener() {
            @Override public void onClick(View v) {
                final boolean needsTarget = action != RETURN_CAMERA && action != COMPLETE_TASKS;
                final int target = needsTarget ? selectedPlayerId() : -1;
                if (needsTarget && target < 0) {
                    setActionMessage("No valid target; enter a match and choose a player");
                    return;
                }
                boolean queued;
                try {
                    queued = nativeAction(action, target);
                } catch (Throwable t) {
                    queued = false;
                }
                setActionMessage(queued ? "Action queued for the next Unity tick" : "Action unavailable or already pending");
                MAIN.postDelayed(new Runnable() {
                    @Override public void run() { refreshActionInfo(); }
                }, 200);
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = px(8, dp);
        b.setLayoutParams(lp);
        return b;
    }

    private static String rowPart(String row, int field) {
        if (row == null) return "";
        int start = 0;
        for (int i = 0; i < field; ++i) {
            start = row.indexOf('\t', start);
            if (start < 0) return "";
            ++start;
        }
        final int end = row.indexOf('\t', start);
        return row.substring(start, end < 0 ? row.length() : end);
    }

    private static int selectedPlayerId() {
        if (selectedPlayer < 0 || selectedPlayer >= playerRows.length) return -1;
        try {
            return Integer.parseInt(rowPart(playerRows[selectedPlayer], 0));
        } catch (NumberFormatException e) {
            return -1;
        }
    }

    private static void updateTargetText() {
        if (targetText == null) return;
        if (playerRows.length == 0) {
            targetText.setText("Target: enter a match");
            selectedPlayer = 0;
        } else {
            selectedPlayer = Math.max(0, Math.min(selectedPlayer, playerRows.length - 1));
            final String row = playerRows[selectedPlayer];
            targetText.setText("#" + rowPart(row, 0) + " " + rowPart(row, 3) +
                    ("1".equals(rowPart(row, 2)) ? " · DEAD" : ""));
        }
        updateActionAvailability(lastStatus);
    }

    private static void setActionMessage(String message) {
        if (actionInfo != null) actionInfo.setText(message);
    }

    private static void refreshPlayers() {
        try {
            final int previousId = selectedPlayerId();
            final String[] incoming = nativePlayers();
            if (incoming != null) {
                int count = 0;
                for (String row : incoming)
                    if (row != null && !"1".equals(rowPart(row, 1))) ++count;
                final String[] rows = new String[count];
                int at = 0;
                for (String row : incoming)
                    if (row != null && !"1".equals(rowPart(row, 1))) rows[at++] = row;
                playerRows = rows;
            }
            selectedPlayer = playerRows.length == 0 ? 0 : Math.min(selectedPlayer, playerRows.length - 1);
            if (previousId >= 0) {
                for (int i = 0; i < playerRows.length; ++i) {
                    if (Integer.toString(previousId).equals(rowPart(playerRows[i], 0))) {
                        selectedPlayer = i;
                        break;
                    }
                }
            }
        } catch (Throwable t) {
            // Keep the previous snapshot: a transient JNI failure must not clear a selected target.
        }
        updateTargetText();
    }

    private static void refreshActionInfo() {
        if (actionInfo == null) return;
        try {
            final String raw = nativeActionInfo();
            if (raw == null) return;
            final String[] fields = raw.split("\\|", 4);
            if (fields.length >= 4) {
                try { lastActionSequence = Integer.parseInt(fields[0]); } catch (NumberFormatException ignored) {}
                final String prefix = "1".equals(fields[1]) ? "Pending · " :
                        ("1".equals(fields[2]) ? "Spectating · " : "");
                actionInfo.setText(prefix + fields[3]);
            }
        } catch (Throwable ignored) {}
    }

    private static void setActionAvailable(TextView button, boolean available) {
        if (button == null) return;
        button.setEnabled(available);
        button.setAlpha(available ? 1f : 0.35f);
    }

    private static void updateActionAvailability(int st) {
        final boolean inGame = (st & ST_LOCAL) != 0;
        final boolean hasTarget = playerRows.length > 0;
        setActionAvailable(previousPlayer, inGame && (st & ST_PLAYERS) != 0 && hasTarget);
        setActionAvailable(nextPlayer, inGame && (st & ST_PLAYERS) != 0 && hasTarget);
        setActionAvailable(teleportButton, inGame && hasTarget && (st & ST_TELEPORT) != 0);
        setActionAvailable(bringButton, inGame && hasTarget && (st & ST_BRING) != 0);
        setActionAvailable(spectateButton, inGame && hasTarget && (st & ST_SPECTATE) != 0);
        setActionAvailable(returnCameraButton, inGame && (st & ST_SPECTATE) != 0);
        setActionAvailable(reportButton, inGame && hasTarget && (st & ST_REPORT) != 0);
        setActionAvailable(completeTasksButton, inGame && (st & ST_COMPLETE_TASKS) != 0);
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

    private static int parentWidth() { return parentView == null ? 0 : parentView.getWidth(); }
    private static int parentHeight() { return parentView == null ? 0 : parentView.getHeight(); }
    private static int fabSize() {
        if (fabHost == null) return 0;
        if (fabHost.getWidth() > 0) return fabHost.getWidth();
        return px(FAB_DP, fabHost.getResources().getDisplayMetrics().density);
    }
    private static float freeWidth() { return Math.max(0, parentWidth() - fabSize()); }
    private static float freeHeight() { return Math.max(0, parentHeight() - fabSize()); }

    private static void applyFab(float x, float y) {
        if (fabHost == null) return;
        fabX = x;
        fabY = y;
        if (!windowMode) {
            fabHost.setX(x);
            fabHost.setY(y);
            return;
        }
        if (wm == null || fabLp == null || !fabAdded) return;
        final int ix = Math.round(x), iy = Math.round(y);
        if (fabLp.x == ix && fabLp.y == iy) return;
        fabLp.x = ix;
        fabLp.y = iy;
        try {
            wm.updateViewLayout(fabHost, fabLp);
        } catch (Throwable t) {
            log("moving the FAB window failed: " + t);
        }
    }

    private static void applyPanel(float x, float y) {
        if (panel == null) return;
        if (!windowMode) {
            panel.setX(x);
            panel.setY(y);
            return;
        }
        if (wm == null || panelLp == null || !panelAdded) return;
        panelLp.x = Math.round(x);
        panelLp.y = Math.round(y);
        try {
            wm.updateViewLayout(panel, panelLp);
        } catch (Throwable t) {
            log("moving the panel window failed: " + t);
        }
    }

    private static void placeFab() {
        if (fabHost == null) return;
        applyFab(clamp(fabFx, 0f, 1f) * freeWidth(), clamp(fabFy, 0f, 1f) * freeHeight());
    }

    private static void snapFab(float dp) {
        if (fabHost == null) return;
        final float margin = 10 * dp;
        final float fw = freeWidth(), fh = freeHeight();
        final float tx = fabX + fabSize() / 2f < parentWidth() / 2f ? margin : Math.max(margin, fw - margin);
        final float ty = clamp(fabY, margin, Math.max(margin, fh - margin));
        fabFx = fw > 0 ? tx / fw : 0f;
        fabFy = fh > 0 ? ty / fh : 0f;
        // A WindowManager transaction per animation frame used to race raw-input drag movement.
        // One final layout keeps the FAB and its touch target in the same place.
        applyFab(tx, ty);
    }

    private static void placePanel() {
        if (panel == null || fabHost == null || parentView == null) return;
        final float dp = panel.getResources().getDisplayMetrics().density;
        int pw = panel.getWidth(), ph = panel.getHeight();
        if (pw == 0 || ph == 0) {
            // Not laid out yet (its window is hidden until now): measure it the way its window will.
            panel.measure(View.MeasureSpec.makeMeasureSpec(px(PANEL_DP, dp), View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(Math.max(1, parentHeight()), View.MeasureSpec.AT_MOST));
            pw = panel.getMeasuredWidth();
            ph = panel.getMeasuredHeight();
        }
        final float gap = 10 * dp, margin = 8 * dp;
        final int fs = fabSize(), w = parentWidth(), h = parentHeight();
        final boolean fabOnLeft = fabX + fs / 2f < w / 2f;
        float x = fabOnLeft ? fabX + fs + gap : fabX - pw - gap;
        x = clamp(x, margin, Math.max(margin, w - pw - margin));
        float y = clamp(fabY, margin, Math.max(margin, h - ph - margin));
        applyPanel(x, y);
    }

    private static void openPanel() {
        if (panel == null) return;
        panelOpen = true;
        placePanel();
        // Do not animate alpha/scale or issue a transaction per frame on this size-compat window.
        panel.animate().cancel();
        panel.setAlpha(1f);
        panel.setScaleX(1f);
        panel.setScaleY(1f);
        panel.setVisibility(View.VISIBLE);
        refreshStatus();
        refreshPlayers();
        refreshActionInfo();
        logPanelState("open immediate");
    }

    private static void closePanel() {
        if (panel == null) return;
        panelOpen = false;
        panel.animate().cancel();
        panel.setAlpha(1f);
        panel.setScaleX(1f);
        panel.setScaleY(1f);
        panel.setVisibility(View.INVISIBLE);
    }
    // ---------------------------------------------------------------- status

    private static void refreshStatus() {
        if (statusPill == null) return;
        final int st = nativeStatus();
        lastStatus = st;
        if (speedRow != null) speedRow.setAvailable((st & ST_SPEED) != 0);
        if (lightRow != null) lightRow.setAvailable((st & ST_LIGHT) != 0);
        if (noclipRow != null) noclipRow.setAvailable((st & ST_NOCLIP) != 0);
        if (killRow != null) killRow.setAvailable((st & ST_KILL) != 0);
        if (zoomRow != null) zoomRow.setAvailable((st & ST_ZOOM) != 0);
        if (revealRow != null) revealRow.setAvailable((st & ST_REVEAL) != 0);
        if (ghostsRow != null) ghostsRow.setAvailable((st & ST_GHOSTS) != 0);
        if (wallVisionRow != null) wallVisionRow.setAvailable((st & ST_WALL_VISION) != 0);
        if (invisibleRow != null) invisibleRow.setAvailable((st & ST_INVISIBLE) != 0);
        final boolean inGame = (st & ST_LOCAL) != 0;
        final float dp = statusPill.getResources().getDisplayMetrics().density;
        statusPill.setText(inGame ? "IN GAME" : "LOBBY");
        statusPill.setTextColor(inGame ? OK : MUTED);
        statusPill.setBackground(rounded(inGame ? 0x263DD68C : 0x1AFFFFFF, 10 * dp, 0, 0));
        updateActionAvailability(st);
    }

    private static void schedulePoll() {
        if (polling) return;
        polling = true;
        MAIN.postDelayed(new Runnable() {
            @Override public void run() {
                if (fabHost == null) {
                    polling = false;
                    return;
                }
                refreshStatus();
                if (panelOpen) refreshPlayers();
                refreshActionInfo();
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
            pos = target;
            invalidate();
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
     * ScrollView capped at ~62% of the parent height, so the panel fits in landscape. The parent is
     * the app's real area; DisplayMetrics can describe the display on a size-compat window.
     */
    private static final class MaxHeightScroll extends ScrollView {
        MaxHeightScroll(Context c) {
            super(c);
            setVerticalScrollBarEnabled(false);
            setOverScrollMode(View.OVER_SCROLL_NEVER);
        }

        @Override protected void onMeasure(int wSpec, int hSpec) {
            final int avail = parentHeight() > 0 ? parentHeight() : getResources().getDisplayMetrics().heightPixels;
            int max = Math.round(avail * 0.62f);
            super.onMeasure(wSpec, MeasureSpec.makeMeasureSpec(max, MeasureSpec.AT_MOST));
        }
    }

    /** Panel root: logs the first touches that reach it, so "no input" and "input missed" can be told apart. */
    private static final class PanelLayout extends LinearLayout {
        PanelLayout(Context c) { super(c); }

        @Override public boolean dispatchTouchEvent(MotionEvent e) {
            if (e.getActionMasked() == MotionEvent.ACTION_DOWN) logTouch("panel", e);
            return super.dispatchTouchEvent(e);
        }
    }
}
