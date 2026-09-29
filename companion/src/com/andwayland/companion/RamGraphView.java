package com.andwayland.companion;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.DashPathEffect;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * Minimalist, elegant sparkline graph for real-time memory monitoring.
 * Clean, subtle monochrome rendering following the minimalist-ui design protocol.
 */
public class RamGraphView extends View {

    private static final int MAX_DATA_POINTS = 35;
    private final List<Float> dataPoints = new ArrayList<>();

    private final Paint linePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fillPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint gridPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dotOuterPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint dotInnerPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    private final Path linePath = new Path();
    private final Path fillPath = new Path();

    public RamGraphView(Context context) {
        super(context);
        init();
    }

    public RamGraphView(Context context, AttributeSet attrs) {
        super(context, attrs);
        init();
    }

    public RamGraphView(Context context, AttributeSet attrs, int defStyleAttr) {
        super(context, attrs, defStyleAttr);
        init();
    }

    private void init() {
        float density = getResources().getDisplayMetrics().density;

        linePaint.setColor(Color.parseColor("#AEAEB2"));
        linePaint.setStyle(Paint.Style.STROKE);
        linePaint.setStrokeWidth(1.5f * density);
        linePaint.setStrokeCap(Paint.Cap.ROUND);
        linePaint.setStrokeJoin(Paint.Join.ROUND);

        fillPaint.setStyle(Paint.Style.FILL);

        gridPaint.setColor(Color.parseColor("#1C1C20"));
        gridPaint.setStyle(Paint.Style.STROKE);
        gridPaint.setStrokeWidth(0.8f * density);
        gridPaint.setPathEffect(new DashPathEffect(new float[]{3f * density, 3f * density}, 0));

        dotOuterPaint.setColor(Color.parseColor("#20FFFFFF"));
        dotOuterPaint.setStyle(Paint.Style.FILL);

        dotInnerPaint.setColor(Color.parseColor("#EAEAEA"));
        dotInnerPaint.setStyle(Paint.Style.FILL);

        // Prepopulate with a baseline
        for (int i = 0; i < 15; i++) {
            dataPoints.add(32.0f);
        }
    }

    public synchronized void addDataPoint(float valueMb) {
        if (dataPoints.size() >= MAX_DATA_POINTS) {
            dataPoints.remove(0);
        }
        dataPoints.add(valueMb);
        postInvalidate();
    }

    public synchronized void setDataPoints(List<Float> points) {
        dataPoints.clear();
        if (points != null) {
            dataPoints.addAll(points);
        }
        postInvalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);

        int width = getWidth();
        int height = getHeight();
        if (width <= 0 || height <= 0 || dataPoints.isEmpty()) {
            return;
        }

        float paddingTop = 8f;
        float paddingBottom = 12f;
        float usableHeight = height - paddingTop - paddingBottom;

        // Subtle horizontal grid reference lines
        for (int i = 1; i <= 3; i++) {
            float y = paddingTop + (usableHeight * i / 4f);
            canvas.drawLine(0, y, width, y, gridPaint);
        }

        // Determine range with comfortable margin
        float minVal = Float.MAX_VALUE;
        float maxVal = Float.MIN_VALUE;
        synchronized (this) {
            for (float val : dataPoints) {
                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
            }
        }

        if (maxVal - minVal < 10.0f) {
            minVal = Math.max(0, minVal - 5.0f);
            maxVal = maxVal + 10.0f;
        } else {
            float margin = (maxVal - minVal) * 0.15f;
            minVal = Math.max(0, minVal - margin);
            maxVal = maxVal + margin;
        }

        float valRange = maxVal - minVal;
        if (valRange <= 0.001f) valRange = 1.0f;

        linePath.reset();
        fillPath.reset();

        int n = dataPoints.size();
        float stepX = (float) width / Math.max(1, n - 1);

        float firstX = 0;
        float firstY = paddingTop + usableHeight * (1.0f - (dataPoints.get(0) - minVal) / valRange);

        linePath.moveTo(firstX, firstY);
        fillPath.moveTo(firstX, height);
        fillPath.lineTo(firstX, firstY);

        float lastX = firstX;
        float lastY = firstY;

        for (int i = 1; i < n; i++) {
            float x = i * stepX;
            float y = paddingTop + usableHeight * (1.0f - (dataPoints.get(i) - minVal) / valRange);

            float prevX = (i - 1) * stepX;
            float prevY = paddingTop + usableHeight * (1.0f - (dataPoints.get(i - 1) - minVal) / valRange);

            float cx1 = prevX + (x - prevX) / 2.0f;
            float cy1 = prevY;
            float cx2 = prevX + (x - prevX) / 2.0f;
            float cy2 = y;

            linePath.cubicTo(cx1, cy1, cx2, cy2, x, y);
            fillPath.cubicTo(cx1, cy1, cx2, cy2, x, y);

            lastX = x;
            lastY = y;
        }

        fillPath.lineTo(lastX, height);
        fillPath.close();

        // Subtle monochrome gradient fill
        LinearGradient gradient = new LinearGradient(
                0, 0, 0, height,
                Color.parseColor("#12FFFFFF"),
                Color.parseColor("#00000000"),
                Shader.TileMode.CLAMP
        );
        fillPaint.setShader(gradient);

        canvas.drawPath(fillPath, fillPaint);
        canvas.drawPath(linePath, linePaint);

        // Indicator dot on current value
        float density = getResources().getDisplayMetrics().density;
        canvas.drawCircle(lastX, lastY, 5f * density, dotOuterPaint);
        canvas.drawCircle(lastX, lastY, 2.5f * density, dotInnerPaint);
    }
}
