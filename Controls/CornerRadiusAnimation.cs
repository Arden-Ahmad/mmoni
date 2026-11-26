using System;
using System.Windows;
using System.Windows.Media.Animation;

namespace Mmoni.Controls;

/// <summary>
/// Smoothly animates a Border's CornerRadius between pill, peek, and dashboard radii.
/// </summary>
internal sealed class CornerRadiusAnimation : AnimationTimeline
{
    public CornerRadius From { get; set; }
    public CornerRadius To { get; set; }
    public IEasingFunction? EasingFunction { get; set; }

    public CornerRadiusAnimation() { }

    public CornerRadiusAnimation(CornerRadius from, CornerRadius to, Duration duration)
    {
        From = from;
        To = to;
        Duration = duration;
    }

    public override Type TargetPropertyType => typeof(CornerRadius);

    protected override Freezable CreateInstanceCore() => new CornerRadiusAnimation
    {
        From = From,
        To = To,
        Duration = Duration,
        EasingFunction = EasingFunction,
        FillBehavior = FillBehavior
    };

    public override object GetCurrentValue(object defaultOriginValue, object defaultDestinationValue, AnimationClock animationClock)
    {
        var progress = animationClock.CurrentProgress ?? 0.0;
        if (EasingFunction is not null)
            progress = EasingFunction.Ease(progress);

        return new CornerRadius(
            Lerp(From.TopLeft, To.TopLeft, progress),
            Lerp(From.TopRight, To.TopRight, progress),
            Lerp(From.BottomRight, To.BottomRight, progress),
            Lerp(From.BottomLeft, To.BottomLeft, progress));
    }

    private static double Lerp(double a, double b, double t) => Math.Max(0, a + (b - a) * t);
}
