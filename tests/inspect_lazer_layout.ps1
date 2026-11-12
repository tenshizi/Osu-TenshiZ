param([string]$InstallPath = "$env:LOCALAPPDATA\osulazer\current", [string[]]$TypeNames = @('osu.Game.OsuGameBase', 'osu.Game.OsuGame', 'osu.Framework.Timing.FramedClock', 'osu.Game.Beatmaps.FramedBeatmapClock', 'osu.Game.Screens.Play.Player', 'osu.Game.Screens.Play.GameplayClockContainer', 'osu.Game.Screens.Play.OffsetCorrectionClock'))
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Runtime.CompilerServices;
using System.Runtime.Loader;
using System.Linq;
public static class LazerLayoutInspection {
    public static void Run(string path, string[] names) {
        AssemblyLoadContext.Default.Resolving += (ctx, name) => {
            string file = Path.Combine(path, name.Name + ".dll");
            return File.Exists(file) ? ctx.LoadFromAssemblyPath(file) : null;
        };
        var assemblies = new [] { "osu.Framework.dll", "osu.Game.dll", "osu.Game.Rulesets.Osu.dll" }
            .Select(a => AssemblyLoadContext.Default.LoadFromAssemblyPath(Path.Combine(path, a)))
            .Concat(new [] { typeof(object).Assembly }).ToArray();
        foreach (var asm in assemblies) {
            foreach (string name in names) {
                var type = asm.GetType(name);
                if (type == null) continue;
                if (type.IsGenericTypeDefinition && type.GetGenericArguments().Length == 1) {
                    var argument = name.StartsWith("osu.Framework.Graphics.")
                        ? assemblies[2].GetType("osu.Game.Rulesets.Osu.Objects.Drawables.DrawableHitCircle")
                        : assemblies[2].GetType("osu.Game.Rulesets.Osu.Objects.OsuHitObject");
                    if (name == "osu.Game.Rulesets.Objects.Pooling.PoolableDrawableWithLifetime`1")
                        argument = assemblies[1].GetType("osu.Game.Rulesets.Objects.HitObjectLifetimeEntry");
                    if (name == "osu.Framework.Layout.LayoutValue`1")
                        argument = assemblies[0].GetType("osu.Framework.Graphics.DrawInfo");
                    if (name == "osu.Framework.Bindables.Bindable`1")
                        argument = typeof(bool);
                    type = type.MakeGenericType(argument);
                }
                else if (type.IsGenericTypeDefinition && type.GetGenericArguments().Length == 2)
                    type = type.MakeGenericType(assemblies[1].GetType("osu.Game.Rulesets.Objects.HitObjectLifetimeEntry"), assemblies[2].GetType("osu.Game.Rulesets.Osu.Objects.Drawables.DrawableHitCircle"));
                Console.WriteLine("TYPE " + name);
                Console.WriteLine("BASE " + type.BaseType);
                var concrete = type;
                if (type.IsAbstract) {
                    concrete = new [] { "osu.Game.Screens.Play.SoloPlayer", "osu.Game.Rulesets.Osu.Objects.HitCircle", "osu.Game.Rulesets.Osu.Objects.Drawables.DrawableHitCircle", "osu.Game.Rulesets.Osu.UI.DrawableOsuRuleset", "osu.Game.Rulesets.Osu.UI.OsuPlayfield", "osu.Game.Rulesets.UI.HitObjectContainer", "osu.Framework.Graphics.Containers.Container" }
                        .SelectMany(n => assemblies.Select(a => a.GetType(n))).FirstOrDefault(t => t != null && !t.IsAbstract && type.IsAssignableFrom(t));
                }
                if (concrete == null || concrete.IsAbstract) { Console.WriteLine("No concrete instance type"); continue; }
                var instance = RuntimeHelpers.GetUninitializedObject(concrete);
                foreach (var field in type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly)) {
                    var method = new DynamicMethod("offset", typeof(long), new [] { typeof(object) }, type, true);
                    var il = method.GetILGenerator();
                    il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Castclass, type);
                    il.Emit(OpCodes.Ldflda, field); il.Emit(OpCodes.Conv_I8);
                    il.Emit(OpCodes.Ldarg_0); il.Emit(OpCodes.Conv_I8); il.Emit(OpCodes.Sub); il.Emit(OpCodes.Ret);
                    var getOffset = (Func<object,long>)method.CreateDelegate(typeof(Func<object,long>));
                    Console.WriteLine("{0:X4} {1} {2}", getOffset(instance), field.FieldType.FullName, field.Name);
                }
            }
        }
    }
}
'@
[LazerLayoutInspection]::Run([System.IO.Path]::GetFullPath($InstallPath), $TypeNames)
