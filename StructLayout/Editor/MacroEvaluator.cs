﻿using EnvDTE;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.VCProjectEngine;
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;

namespace StructLayout
{
    interface IMacroEvaluator
    {
        string Evaluate(string input);
    }

    public class MacroEvaluatorVisualPlatform : IMacroEvaluator
    {
        private VCPlatform Platform { set; get; }

        public MacroEvaluatorVisualPlatform(VCPlatform platform)
        {
            Platform = platform;
        }

        public string Evaluate(string input)
        {
            return Platform.Evaluate(input);
        }
    }

    public abstract class MacroEvaluatorDict : IMacroEvaluator
    {
        private Dictionary<string, string> dict = new Dictionary<string, string>();

        public abstract string ComputeMacro(string macroStr);      

        public string Evaluate(string input)
        {
            return Regex.Replace(input, @"(\$\([a-zA-Z_]+\))", delegate (Match m)
            {                
                if (dict.ContainsKey(m.Value))
                {
                    return dict[m.Value];
                }

                string macroValue = ComputeMacro(m.Value);

                if (macroValue != null)
                {
                    dict[m.Value] = macroValue;
                    return macroValue;
                }

                return m.Value;
            });
        }
    }

    public class MacroEvaluatorBasic : MacroEvaluatorDict
    {
        public override string ComputeMacro(string macroStr)
        {
            ThreadHelper.ThrowIfNotOnUIThread();

            if (macroStr == @"$(SolutionDir)")
            {
                Solution solution = EditorUtils.GetActiveSolution();
                return solution == null? null : Path.GetDirectoryName(solution.FullName) + '\\' ;
            }
            else if (macroStr == @"$(ProjectDir)")
            {
                Project project = EditorUtils.GetActiveProject();
                return project == null? null : Path.GetDirectoryName(project.FullName) + '\\';
            }
            else if (macroStr == @"$(Configuration)")
            {
                Project project = EditorUtils.GetActiveProject();
                if (project != null)
                {
                    Configuration config = project.ConfigurationManager.ActiveConfiguration;
                    return config == null ? null : config.ConfigurationName;
                }
            }
            else if (macroStr == @"$(Platform)")
            {
                Project project = EditorUtils.GetActiveProject();
                if (project != null)
                {
                    Configuration config = project.ConfigurationManager.ActiveConfiguration;
                    return config == null ? null : config.PlatformName;
                }
            }

            return null;
        }
    }

    public class MacroEvaluatorExtra : MacroEvaluatorDict
    {
        public override string ComputeMacro(string macroStr)
        {
            //TODO ~ ramonv ~ to be implemented
     
            //Add UE4ModuleName

            return null;
        }
    }
}