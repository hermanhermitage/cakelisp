#include "Evaluator.hpp"

#include "Converters.hpp"
#include "DynamicLoader.hpp"
#include "FileUtilities.hpp"
#include "GeneratorHelpers.hpp"
#include "Generators.hpp"
#include "Logging.hpp"
#include "OutputPreambles.hpp"
#include "RunProcess.hpp"
#include "Tokenizer.hpp"
#include "Utilities.hpp"
#include "Writer.hpp"

// TODO: safe version of strcat
#include <stdio.h>
#include <string.h>

//
// Environment
//

const char* globalDefinitionName = "<global>";
const char* cakelispWorkingDir = "cakelisp_cache";

GeneratorFunc findGenerator(EvaluatorEnvironment& environment, const char* functionName)
{
	GeneratorIterator findIt = environment.generators.find(std::string(functionName));
	if (findIt != environment.generators.end())
		return findIt->second;
	return nullptr;
}

MacroFunc findMacro(EvaluatorEnvironment& environment, const char* functionName)
{
	MacroIterator findIt = environment.macros.find(std::string(functionName));
	if (findIt != environment.macros.end())
		return findIt->second;
	return nullptr;
}

StateVariable* findModuleStateVariable(ModuleEnvironment* moduleEnvironment,
                                       const char* stateVariableName)
{
	StateVariableIterator findIt =
	    moduleEnvironment->stateVariables.find(std::string(stateVariableName));
	if (findIt != moduleEnvironment->stateVariables.end())
		return &findIt->second;
	return nullptr;
}

bool isCompileTimeCodeLoaded(EvaluatorEnvironment& environment, const ObjectDefinition& definition)
{
	if (definition.type == ObjectType_CompileTimeMacro)
		return findMacro(environment, definition.name->contents.c_str()) != nullptr;
	else
		return findGenerator(environment, definition.name->contents.c_str()) != nullptr;
}

bool addObjectDefinition(EvaluatorEnvironment& environment, ObjectDefinition& definition)
{
	ObjectDefinitionMap::iterator findIt = environment.definitions.find(definition.name->contents);
	if (findIt == environment.definitions.end())
	{
		if (findGenerator(environment, definition.name->contents.c_str()) ||
		    findMacro(environment, definition.name->contents.c_str()))
		{
			ErrorAtTokenf(*definition.name,
			              "multiple definitions of %s. Name may be conflicting with built-in macro "
			              "or generator",
			              definition.name->contents.c_str());
			return false;
		}

		environment.definitions[definition.name->contents] = definition;
		return true;
	}
	else
	{
		ErrorAtTokenf(*definition.name, "multiple definitions of %s",
		              definition.name->contents.c_str());
		NoteAtToken(*findIt->second.name, "first defined here");
		return false;
	}
}

const ObjectReferenceStatus* addObjectReference(EvaluatorEnvironment& environment,
                                                const Token& referenceNameToken,
                                                ObjectReference& reference)
{
	// Default to the module requiring the reference, for top-level references
	std::string definitionName = globalDefinitionName;
	if (!reference.context.definitionName && reference.context.scope != EvaluatorScope_Module)
		printf("error: addObjectReference() expects a definitionName\n");

	if (reference.context.definitionName)
	{
		definitionName = reference.context.definitionName->contents;
	}

	const char* defName = definitionName.c_str();
	if (log.references)
		printf("Adding reference %s to %s\n", referenceNameToken.contents.c_str(), defName);

	// Add the reference requirement to the definition it occurred in
	ObjectReferenceStatus* refStatus = nullptr;
	ObjectDefinitionMap::iterator findDefinition = environment.definitions.find(definitionName);
	if (findDefinition == environment.definitions.end())
	{
		if (definitionName.compare(globalDefinitionName) != 0)
		{
			printf("error: expected definition %s to already exist. Things will break\n",
			       definitionName.c_str());
		}
		else
		{
			ErrorAtTokenf(referenceNameToken,
			              "error: expected %s definition to exist as a top-level catch-all",
			              globalDefinitionName);
		}
	}
	else
	{
		// The reference is copied here somewhat unnecessarily. It would be too much of a hassle to
		// make a good link to the reference in the reference pool, because it can easily be moved
		// by hash realloc or vector resize
		ObjectReferenceStatusMap::iterator findRefIt =
		    findDefinition->second.references.find(referenceNameToken.contents.c_str());
		if (findRefIt == findDefinition->second.references.end())
		{
			ObjectReferenceStatus newStatus;
			newStatus.name = &referenceNameToken;
			newStatus.guessState = GuessState_None;
			newStatus.references.push_back(reference);
			std::pair<ObjectReferenceStatusMap::iterator, bool> newRefStatusResult =
			    findDefinition->second.references.emplace(
			        std::make_pair(referenceNameToken.contents, std::move(newStatus)));
			refStatus = &newRefStatusResult.first->second;
		}
		else
		{
			findRefIt->second.references.push_back(reference);
			refStatus = &findRefIt->second;
		}
	}

	// Add the reference to the reference pool. This makes it easier to find all places where it is
	// referenced during resolve time
	ObjectReferencePoolMap::iterator findIt =
	    environment.referencePools.find(referenceNameToken.contents);
	if (findIt == environment.referencePools.end())
	{
		ObjectReferencePool newPool = {};
		newPool.references.push_back(reference);
		environment.referencePools[referenceNameToken.contents] = std::move(newPool);
	}
	else
	{
		findIt->second.references.push_back(reference);
	}

	return refStatus;
}

int getNextFreeBuildId(EvaluatorEnvironment& environment)
{
	return ++environment.nextFreeBuildId;
}

bool isCompileTimeObject(ObjectType type)
{
	return type == ObjectType_CompileTimeMacro || type == ObjectType_CompileTimeGenerator;
}

//
// Evaluator
//

// Dispatch to a generator or expand a macro and evaluate its output recursively. If the reference
// is unknown, add it to a list so EvaluateResolveReferences() can come back and decide what to do
// with it. Only EvaluateResolveReferences() decides whether to create a C/C++ invocation
bool HandleInvocation_Recursive(EvaluatorEnvironment& environment, const EvaluatorContext& context,
                                const std::vector<Token>& tokens, int invocationStartIndex,
                                GeneratorOutput& output)
{
	const Token& invocationStart = tokens[invocationStartIndex];
	const Token& invocationName = tokens[invocationStartIndex + 1];
	if (!ExpectTokenType("evaluator", invocationName, TokenType_Symbol))
		return false;

	MacroFunc invokedMacro = findMacro(environment, invocationName.contents.c_str());
	if (invokedMacro)
	{
		// We must use a separate vector for each macro because Token lists must be immutable. If
		// they weren't, pointers to tokens would be invalidated
		const std::vector<Token>* macroOutputTokens = nullptr;
		bool macroSucceeded;
		{
			// Do NOT modify token lists after they are created. You can change the token contents
			std::vector<Token>* macroOutputTokensNoConst_CREATIONONLY = new std::vector<Token>();

			// Have the macro generate some code for us!
			macroSucceeded = invokedMacro(environment, context, tokens, invocationStartIndex,
			                              *macroOutputTokensNoConst_CREATIONONLY);

			// Make it const to save any temptation of modifying the list and breaking everything
			macroOutputTokens = macroOutputTokensNoConst_CREATIONONLY;
		}

		// Don't even try to validate the code if the macro wasn't satisfied
		if (!macroSucceeded)
		{
			ErrorAtToken(invocationName, "macro returned failure");

			// Deleting these tokens is only safe at this point because we know we have not
			// evaluated them. As soon as they are evaluated, they must be kept around
			delete macroOutputTokens;
			return false;
		}

		// The macro had no output, but we won't let that bother us
		if (macroOutputTokens->empty())
		{
			delete macroOutputTokens;
			return true;
		}

		// TODO: Pretty print to macro expand file and change output token source to
		// point there

		// Macro must generate valid parentheses pairs!
		bool validateResult = validateParentheses(*macroOutputTokens);
		if (!validateResult)
		{
			NoteAtToken(invocationStart,
			            "code was generated from macro. See erroneous macro "
			            "expansion below:");
			prettyPrintTokens(*macroOutputTokens);
			printf("\n");
			// Deleting these tokens is only safe at this point because we know we have not
			// evaluated them. As soon as they are evaluated, they must be kept around
			delete macroOutputTokens;
			return false;
		}

		// Macro succeeded and output valid tokens. Keep its tokens for later referencing and
		// destruction. Note that macroOutputTokens cannot be destroyed safely until all pointers to
		// its Tokens are cleared. This means even if we fail while evaluating the tokens, we will
		// keep the array around because the environment might still hold references to the tokens.
		// It's also necessary for error reporting
		environment.macroExpansions.push_back(macroOutputTokens);

		// Note that macros always inherit the current context, whereas bodies change it
		int result = EvaluateGenerateAll_Recursive(environment, context, *macroOutputTokens,
		                                           /*startTokenIndex=*/0,
		                                           /*delimiterTemplate=*/nullptr, output);
		if (result != 0)
		{
			NoteAtToken(invocationStart,
			            "code was generated from macro. See macro expansion below:");
			prettyPrintTokens(*macroOutputTokens);
			printf("\n");
			return false;
		}

		return true;
	}

	GeneratorFunc invokedGenerator = findGenerator(environment, invocationName.contents.c_str());
	if (invokedGenerator)
	{
		return invokedGenerator(environment, context, tokens, invocationStartIndex, output);
	}

	// Check for known Cakelisp functions
	ObjectDefinitionMap::iterator findIt = environment.definitions.find(invocationName.contents);
	if (findIt != environment.definitions.end() && !isCompileTimeObject(findIt->second.type))
	{
		return FunctionInvocationGenerator(environment, context, tokens, invocationStartIndex,
		                                   output);
	}

	// Unknown reference
	{
		// We don't know what this is. We cannot guess it is a C/C++ function yet, because it
		// could be a generator or macro invocation that hasn't been defined yet. Leave a note
		// for the evaluator to come back to this token once a satisfying answer is found
		ObjectReference newReference = {};
		newReference.tokens = &tokens;
		newReference.startIndex = invocationStartIndex;
		newReference.context = context;
		// Make room for whatever gets output once this reference is resolved
		newReference.spliceOutput = new GeneratorOutput;

		// We push in a StringOutMod_Splice as a sentinel that the splice list needs to be
		// checked. Otherwise, it will be a no-op to Writer. It's useful to have this sentinel
		// so that multiple splices take up space and will then maintain sequential order
		addSpliceOutput(output.source, newReference.spliceOutput, &invocationStart);

		const ObjectReferenceStatus* referenceStatus =
		    addObjectReference(environment, invocationName, newReference);
		if (!referenceStatus)
		{
			ErrorAtToken(tokens[invocationStartIndex],
			             "failed to create reference status (internal error)");
			return false;
		}

		// If some action has already happened on this reference, duplicate it here
		// This code wouldn't be necessary if BuildEvaluateReferences() checked all of its reference
		// instances, and stored a status on each one. I don't like the duplication here, but it
		// does match the other HandleInvocation_Recursive() invocation types, which are handled as
		// soon as the environment has enough information to resolve the invocation
		if (referenceStatus->guessState == GuessState_Guessed)
		{
			// Guess now, because BuildEvaluateReferences thinks it has already guessed all refs
			bool result =
			    FunctionInvocationGenerator(environment, newReference.context, *newReference.tokens,
			                                newReference.startIndex, *newReference.spliceOutput);
			// Our guess didn't even evaluate
			if (!result)
				return false;
		}

		// We're going to return true even though evaluation isn't yet done.
		// BuildEvaluateReferences() will handle the evaluation after it knows what the
		// references are
	}

	return true;
}

int EvaluateGenerate_Recursive(EvaluatorEnvironment& environment, const EvaluatorContext& context,
                               const std::vector<Token>& tokens, int startTokenIndex,
                               GeneratorOutput& output)
{
	// Note that in most cases, we will continue evaluation in order to turn up more errors
	int numErrors = 0;

	const Token& token = tokens[startTokenIndex];

	if (token.type == TokenType_OpenParen)
	{
		// Invocation of a macro, generator, or function (either foreign or Cakelisp function)
		bool invocationSucceeded =
		    HandleInvocation_Recursive(environment, context, tokens, startTokenIndex, output);
		if (!invocationSucceeded)
			++numErrors;
	}
	else if (token.type == TokenType_CloseParen)
	{
		// This is totally normal. We've reached the end of the body or file. If that isn't the
		// case, the code isn't being validated with validateParentheses(); code which hasn't
		// been validated should NOT be run - this function trusts its inputs blindly!
		// This will also be hit if eval itself has been broken: it is expected to skip tokens
		// within invocations, including the final close paren
		return numErrors;
	}
	else
	{
		// The remaining token types evaluate to themselves. Output them directly.
		if (ExpectEvaluatorScope("evaluated constant or symbol", token, context,
		                         EvaluatorScope_ExpressionsOnly))
		{
			switch (token.type)
			{
				case TokenType_Symbol:
				{
					// We need to convert what look like names in case they are lispy, but not
					// integer, character, or floating point constants
					char firstChar = token.contents[0];
					char secondChar = token.contents.size() > 1 ? token.contents[1] : 0;
					if (firstChar == '\'' || isdigit(firstChar) ||
					    (firstChar == '-' && (secondChar == '.' || isdigit(secondChar))))
					{
						// Add as-is
						addStringOutput(output.source, token.contents, StringOutMod_None, &token);
					}
					else
					{
						if (environment.enableHotReloading && context.moduleEnvironment &&
						    findModuleStateVariable(context.moduleEnvironment,
						                            token.contents.c_str()))
						{
							// StateVariables are automatically converted to pointers, so all
							// accessing of them needs to happen through a dereference
							// Use (no-eval-var my-var) if you need to get around the auto
							// dereference (if you are e.g. writing code which sets the pointer)
							addLangTokenOutput(output.source, StringOutMod_OpenParen, &token);
							addStringOutput(output.source, "*", StringOutMod_None, &token);
							addStringOutput(output.source, token.contents,
							                StringOutMod_ConvertVariableName, &token);
							addLangTokenOutput(output.source, StringOutMod_CloseParen, &token);
						}
						else
						{
							// Potential lisp name. Convert
							addStringOutput(output.source, token.contents,
							                StringOutMod_ConvertVariableName, &token);
						}
					}
					break;
				}
				case TokenType_String:
					addStringOutput(output.source, token.contents, StringOutMod_SurroundWithQuotes,
					                &token);
					break;
				default:
					ErrorAtTokenf(token,
					              "Unhandled token type %s; has a new token type been added, or "
					              "evaluator has been changed?",
					              tokenTypeToString(token.type));
					return 1;
			}
		}
		else
			numErrors++;
	}

	return numErrors;
}

// Delimiter template will be inserted between the outputs
int EvaluateGenerateAll_Recursive(EvaluatorEnvironment& environment,
                                  const EvaluatorContext& context, const std::vector<Token>& tokens,
                                  int startTokenIndex, const StringOutput* delimiterTemplate,
                                  GeneratorOutput& output)
{
	// Note that in most cases, we will continue evaluation in order to turn up more errors
	int numErrors = 0;

	int numTokens = tokens.size();
	for (int currentTokenIndex = startTokenIndex; currentTokenIndex < numTokens;
	     ++currentTokenIndex)
	{
		if (tokens[currentTokenIndex].type == TokenType_CloseParen)
		{
			// Reached the end of an argument list or body. Only modules will hit numTokens
			break;
		}

		// Starting a new argument to evaluate
		if (delimiterTemplate && currentTokenIndex != startTokenIndex)
		{
			StringOutput delimiter = *delimiterTemplate;
			delimiter.startToken = &tokens[currentTokenIndex];
			// TODO: Controlling source vs. header output?
			output.source.push_back(std::move(delimiter));
		}

		numErrors +=
		    EvaluateGenerate_Recursive(environment, context, tokens, currentTokenIndex, output);

		if (tokens[currentTokenIndex].type == TokenType_OpenParen)
		{
			// Skip invocation body. for()'s increment will skip us past the final ')'
			currentTokenIndex = FindCloseParenTokenIndex(tokens, currentTokenIndex);
		}
	}

	return numErrors;
}

const char* objectTypeToString(ObjectType type);

// Determine what needs to be built, iteratively
// TODO This can be made faster. I did the most naive version first, for now
void PropagateRequiredToReferences(EvaluatorEnvironment& environment)
{
	// Figure out what is required
	// This needs to loop so long as it doesn't recurse to references
	int numRequiresStatusChanged = 0;
	do
	{
		numRequiresStatusChanged = 0;
		for (ObjectDefinitionPair& definitionPair : environment.definitions)
		{
			ObjectDefinition& definition = definitionPair.second;
			const char* status = definition.isRequired ? "(required)" : "(not required)";

			if (log.dependencyPropagation)
				printf("Define %s %s\n", definition.name->contents.c_str(), status);

			for (ObjectReferenceStatusPair& reference : definition.references)
			{
				ObjectReferenceStatus& referenceStatus = reference.second;

				if (log.dependencyPropagation)
					printf("\tRefers to %s\n", referenceStatus.name->contents.c_str());

				if (definition.isRequired)
				{
					ObjectDefinitionMap::iterator findIt =
					    environment.definitions.find(referenceStatus.name->contents);
					if (findIt != environment.definitions.end() && !findIt->second.isRequired)
					{
						if (log.dependencyPropagation)
							printf("\t Infecting %s with required due to %s\n",
							       referenceStatus.name->contents.c_str(),
							       definition.name->contents.c_str());

						++numRequiresStatusChanged;
						findIt->second.isRequired = true;
					}
				}
			}
		}
	} while (numRequiresStatusChanged);
}

void OnCompileProcessOutput(const char* output)
{
	// TODO C/C++ error to Cakelisp token mapper
}

int BuildEvaluateReferences(EvaluatorEnvironment& environment, int& numErrorsOut)
{
	int numReferencesResolved = 0;

	enum BuildStage
	{
		BuildStage_None,
		BuildStage_Compiling,
		BuildStage_Linking,
		BuildStage_Loading,
		BuildStage_ResolvingReferences,
		BuildStage_Finished
	};

	// Note: environment.definitions can be resized/rehashed during evaluation, which invalidates
	// iterators. For now, I will rely on the fact that std::unordered_map does not invalidate
	// references on resize. This will need to change if the data structure changes
	struct BuildObject
	{
		int buildId = -1;
		int status = -1;
		BuildStage stage = BuildStage_None;
		std::string artifactsName;
		std::string dynamicLibraryPath;
		std::string buildObjectName;
		ObjectDefinition* definition = nullptr;
	};

	std::vector<BuildObject> definitionsToBuild;

	// We must copy references in case environment.definitions is modified, which would invalidate
	// iterators, but not references
	std::vector<ObjectDefinition*> definitionsToCheck;
	definitionsToCheck.reserve(environment.definitions.size());
	for (ObjectDefinitionPair& definitionPair : environment.definitions)
	{
		ObjectDefinition& definition = definitionPair.second;
		// Does it need to be built?
		if (!definition.isRequired)
			continue;

		// Is it already in the environment?
		if (definition.isLoaded)
			continue;

		definitionsToCheck.push_back(&definition);
	}

	for (ObjectDefinition* definitionPointer : definitionsToCheck)
	{
		ObjectDefinition& definition = *definitionPointer;
		const char* defName = definition.name->contents.c_str();

		if (log.buildReasons)
			printf("Checking to build %s\n", defName);

		// Can it be built in the current environment?
		bool canBuild = true;
		bool hasRelevantChangeOccurred = false;
		bool hasGuessedRefs = false;
		// If there were new guesses, we will do another pass over this definition's references in
		// case new references turned up
		bool guessMaybeDirtiedReferences = false;
		do
		{
			guessMaybeDirtiedReferences = false;

			// Copy pointers to refs in case of iterator invalidation
			std::vector<ObjectReferenceStatus*> referencesToCheck;
			referencesToCheck.reserve(definition.references.size());
			for (ObjectReferenceStatusPair& referencePair : definition.references)
			{
				referencesToCheck.push_back(&referencePair.second);
			}
			for (ObjectReferenceStatus* referencePointer : referencesToCheck)
			{
				ObjectReferenceStatus& referenceStatus = *referencePointer;

				// TODO: (Performance) Add shortcut in reference
				ObjectDefinitionMap::iterator findIt =
				    environment.definitions.find(referenceStatus.name->contents);
				if (findIt != environment.definitions.end())
				{
					if (isCompileTimeObject(findIt->second.type))
					{
						bool refCompileTimeCodeLoaded = findIt->second.isLoaded;
						if (refCompileTimeCodeLoaded)
						{
							// The reference is ready to go. Built objects immediately resolve
							// references. We will react to it if the last thing we did was guess
							// incorrectly that this was a C call
							if (referenceStatus.guessState != GuessState_Resolved)
							{
								if (log.buildReasons)
									printf("\tRequired code has been loaded\n");

								hasRelevantChangeOccurred = true;
							}

							referenceStatus.guessState = GuessState_Resolved;
						}
						else
						{
							// If we know we are missing a compile time function, we won't try to
							// guess
							if (log.buildReasons)
								printf("\tCannot build until %s is loaded\n",
								       referenceStatus.name->contents.c_str());

							referenceStatus.guessState = GuessState_WaitingForLoad;
							canBuild = false;
						}
					}
					else if (findIt->second.type == ObjectType_Function &&
					         referenceStatus.guessState != GuessState_Resolved)
					{
						// A known Cakelisp function call
						for (int i = 0; i < (int)referenceStatus.references.size(); ++i)
						{
							ObjectReference& reference = referenceStatus.references[i];
							// Run function invocation on it
							// TODO: Make invocation generator know it is a Cakelisp function
							bool result = FunctionInvocationGenerator(
							    environment, reference.context, *reference.tokens,
							    reference.startIndex, *reference.spliceOutput);
							// Our guess didn't even evaluate
							if (!result)
								canBuild = false;
						}

						referenceStatus.guessState = GuessState_Resolved;
					}
					// TODO: Building references to non-comptime functions at comptime
				}
				else
				{
					if (referenceStatus.guessState == GuessState_None)
					{
						if (log.buildReasons)
							printf("\tCannot build until %s is guessed. Guessing now\n",
							       referenceStatus.name->contents.c_str());

						// Find all the times the definition makes this reference
						// We must use indices because the call to FunctionInvocationGenerator can
						// add new references to the list
						for (int i = 0; i < (int)referenceStatus.references.size(); ++i)
						{
							ObjectReference& reference = referenceStatus.references[i];
							// Run function invocation on it
							bool result = FunctionInvocationGenerator(
							    environment, reference.context, *reference.tokens,
							    reference.startIndex, *reference.spliceOutput);
							// Our guess didn't even evaluate
							if (!result)
								canBuild = false;
						}

						referenceStatus.guessState = GuessState_Guessed;
						hasRelevantChangeOccurred = true;
						hasGuessedRefs = true;
						guessMaybeDirtiedReferences = true;
					}
					else if (referenceStatus.guessState == GuessState_Guessed)
					{
						// It has been guessed, and still isn't in definitions
						hasGuessedRefs = true;
					}
				}
			}
		} while (guessMaybeDirtiedReferences);

		// hasRelevantChangeOccurred being false suppresses rebuilding compile-time functions which
		// still have the same missing references. Note that only compile time objects can be built.
		// We put normal functions through the guessing system too because they need their functions
		// resolved as well. It's a bit dirty but not too bad
		if (canBuild && (!hasGuessedRefs || hasRelevantChangeOccurred) &&
		    isCompileTimeObject(definition.type))
		{
			BuildObject objectToBuild = {};
			objectToBuild.buildId = getNextFreeBuildId(environment);
			objectToBuild.definition = &definition;
			definitionsToBuild.push_back(objectToBuild);
		}
	}

	// Spin up as many compile processes as necessary
	// TODO: Combine sure-thing builds into batches (ones where we know all references)
	// TODO: Instead of creating files, pipe straight to compiler?
	// TODO: Make pipeline able to start e.g. linker while other objects are still compiling
	// NOTE: definitionsToBuild must not be resized from when runProcess() is called until
	// waitForAllProcessesClosed(), else the status pointer could be invalidated
	const int maxProcessesSpawned = 8;
	int currentNumProcessesSpawned = 0;
	for (BuildObject& buildObject : definitionsToBuild)
	{
		ObjectDefinition* definition = buildObject.definition;

		if (log.buildProcess)
			printf("Build %s\n", definition->name->contents.c_str());

		char convertedNameBuffer[MAX_NAME_LENGTH] = {0};
		lispNameStyleToCNameStyle(NameStyleMode_Underscores, definition->name->contents.c_str(),
		                          convertedNameBuffer, sizeof(convertedNameBuffer),
		                          *definition->name);
		char artifactsName[MAX_PATH_LENGTH] = {0};
		// Various stages will append the appropriate file extension
		PrintfBuffer(artifactsName, "comptime_%s", convertedNameBuffer);
		buildObject.artifactsName = artifactsName;
		char fileOutputName[MAX_PATH_LENGTH] = {0};
		// Writer will append the appropriate file extensions
		PrintfBuffer(fileOutputName, "%s/%s", cakelispWorkingDir,
		             buildObject.artifactsName.c_str());

		// Output definition to a file our compiler will be happy with
		// TODO: Make these come from the top
		NameStyleSettings nameSettings;
		WriterFormatSettings formatSettings;
		WriterOutputSettings outputSettings = {};
		if (definition->type == ObjectType_CompileTimeGenerator)
		{
			outputSettings.sourceHeading = generatorSourceHeading;
			outputSettings.sourceFooter = generatorSourceFooter;
		}
		else
		{
			outputSettings.sourceHeading = macroSourceHeading;
			outputSettings.sourceFooter = macroSourceFooter;
		}
		outputSettings.sourceCakelispFilename = fileOutputName;
		// Use the separate output prepared specifically for this compile-time object
		if (!writeGeneratorOutput(*definition->output, nameSettings, formatSettings,
		                          outputSettings))
		{
			ErrorAtToken(*buildObject.definition->name,
			             "Failed to write to compile-time source file");
			continue;
		}

		buildObject.stage = BuildStage_Compiling;

		char fileToExec[MAX_PATH_LENGTH] = {0};
		PrintBuffer(fileToExec, "/usr/bin/clang++");

		// The evaluator is written in C++, so all generators and macros need to support the C++
		// features used (e.g. their signatures have std::vector<>)
		char sourceOutputName[MAX_PATH_LENGTH] = {0};
		PrintfBuffer(sourceOutputName, "%s/%s.cpp", cakelispWorkingDir,
		             buildObject.artifactsName.c_str());

		char buildObjectName[MAX_PATH_LENGTH] = {0};
		PrintfBuffer(buildObjectName, "%s/%s.o", cakelispWorkingDir,
		             buildObject.artifactsName.c_str());
		buildObject.buildObjectName = buildObjectName;

		char dynamicLibraryOut[MAX_PATH_LENGTH] = {0};
		PrintfBuffer(dynamicLibraryOut, "%s/lib%s.so", cakelispWorkingDir,
		             buildObject.artifactsName.c_str());
		buildObject.dynamicLibraryPath = dynamicLibraryOut;

		if (!fileIsMoreRecentlyModified(sourceOutputName, buildObject.dynamicLibraryPath.c_str()))
		{
			if (log.buildProcess)
				printf("Skipping compiling %s (using cached library)\n", sourceOutputName);
			// Skip straight to linking, which immediately becomes loading
			buildObject.stage = BuildStage_Linking;
			buildObject.status = 0;
			continue;
		}

		// TODO: Get arguments all the way from the top
		// If not null terminated, the call will fail
		const char* arguments[] = {fileToExec,      "-g",     "-c",    sourceOutputName, "-o",
		                           buildObjectName, "-Isrc/", "-fPIC", nullptr};
		RunProcessArguments compileArguments = {};
		compileArguments.fileToExecute = fileToExec;
		compileArguments.arguments = arguments;
		if (runProcess(compileArguments, &buildObject.status) != 0)
		{
			// TODO: Abort building if cannot invoke compiler
			// return 0;
		}

		// TODO This could be made smarter by allowing more spawning right when a process closes,
		// instead of starting in waves
		++currentNumProcessesSpawned;
		if (currentNumProcessesSpawned >= maxProcessesSpawned)
		{
			waitForAllProcessesClosed(OnCompileProcessOutput);
			currentNumProcessesSpawned = 0;
		}
	}

	// The result of the builds will go straight to our definitionsToBuild
	waitForAllProcessesClosed(OnCompileProcessOutput);

	// Linking
	for (BuildObject& buildObject : definitionsToBuild)
	{
		if (buildObject.stage != BuildStage_Compiling)
			continue;

		if (buildObject.status != 0)
		{
			ErrorAtTokenf(*buildObject.definition->name,
			              "Failed to compile definition '%s' with status %d",
			              buildObject.definition->name->contents.c_str(), buildObject.status);
			continue;
		}

		buildObject.stage = BuildStage_Linking;

		if (log.buildProcess)
			printf("Compiled %s successfully\n", buildObject.definition->name->contents.c_str());

		char linkerExecutable[MAX_PATH_LENGTH] = {0};
		PrintBuffer(linkerExecutable, "/usr/bin/clang++");

		const char* arguments[] = {linkerExecutable,
		                           "-shared",
		                           "-o",
		                           buildObject.dynamicLibraryPath.c_str(),
		                           buildObject.buildObjectName.c_str(),
		                           nullptr};
		RunProcessArguments linkArguments = {};
		linkArguments.fileToExecute = linkerExecutable;
		linkArguments.arguments = arguments;
		if (runProcess(linkArguments, &buildObject.status) != 0)
		{
			// TODO: Abort if linker failed?
		}
	}

	// The result of the linking will go straight to our definitionsToBuild
	waitForAllProcessesClosed(OnCompileProcessOutput);

	for (BuildObject& buildObject : definitionsToBuild)
	{
		if (buildObject.stage != BuildStage_Linking)
			continue;

		if (buildObject.status != 0)
		{
			ErrorAtToken(*buildObject.definition->name, "Failed to link definition");
			continue;
		}

		buildObject.stage = BuildStage_Loading;

		if (log.buildProcess)
			printf("Linked %s successfully\n", buildObject.definition->name->contents.c_str());

		DynamicLibHandle builtLib = loadDynamicLibrary(buildObject.dynamicLibraryPath.c_str());
		if (!builtLib)
		{
			ErrorAtToken(*buildObject.definition->name, "Failed to load compile-time library");
			continue;
		}

		// We need to do name conversion to be compatible with C naming
		// TODO: Make these come from the top
		NameStyleSettings nameSettings;
		char symbolNameBuffer[MAX_NAME_LENGTH] = {0};
		lispNameStyleToCNameStyle(nameSettings.functionNameMode,
		                          buildObject.definition->name->contents.c_str(), symbolNameBuffer,
		                          sizeof(symbolNameBuffer), *buildObject.definition->name);
		void* compileTimeFunction = getSymbolFromDynamicLibrary(builtLib, symbolNameBuffer);
		if (!compileTimeFunction)
		{
			ErrorAtToken(*buildObject.definition->name, "Failed to find symbol in loaded library");
			continue;
		}

		// Add to environment
		if (buildObject.definition->type == ObjectType_CompileTimeMacro)
		{
			environment.macros[buildObject.definition->name->contents] =
			    (MacroFunc)compileTimeFunction;
		}
		else if (buildObject.definition->type == ObjectType_CompileTimeGenerator)
		{
			environment.generators[buildObject.definition->name->contents] =
			    (GeneratorFunc)compileTimeFunction;
		}

		buildObject.stage = BuildStage_ResolvingReferences;

		// Resolve references
		ObjectReferencePoolMap::iterator referencePoolIt =
		    environment.referencePools.find(buildObject.definition->name->contents);
		if (referencePoolIt == environment.referencePools.end())
		{
			printf(
			    "Error: built an object which had no references. It should not have been "
			    "required. There must be a problem with Cakelisp internally\n");
			continue;
		}

		// TODO: Performance: Iterate backwards and quit as soon as any resolved refs are found?
		for (ObjectReference& reference : referencePoolIt->second.references)
		{
			if (reference.isResolved)
				continue;

			// In case a compile-time function has already guessed the invocation was a C/C++
			// function, clear that invocation output
			resetGeneratorOutput(*reference.spliceOutput);

			if (log.buildProcess)
				NoteAtToken((*reference.tokens)[reference.startIndex], "resolving reference");

			// Evaluate from that reference
			int result =
			    EvaluateGenerate_Recursive(environment, reference.context, *reference.tokens,
			                               reference.startIndex, *reference.spliceOutput);
			// Regardless of what evaluate turned up, we resolved this as far as we care. Trying
			// again isn't going to change the number of errors
			// Note that if new references emerge to this definition, they will automatically be
			// recognized as the definition and handled then and there, so we don't need to make
			// more than one pass
			reference.isResolved = true;
			numErrorsOut += result;
			++numReferencesResolved;
		}

		if (log.buildProcess)
			printf("Resolved %d references\n", numReferencesResolved);

		// Remove need to build
		buildObject.definition->isLoaded = true;

		buildObject.stage = BuildStage_Finished;

		if (log.buildProcess)
			printf("Successfully built, loaded, and executed %s\n",
			       buildObject.definition->name->contents.c_str());
	}

	return numReferencesResolved;
}

bool EvaluateResolveReferences(EvaluatorEnvironment& environment)
{
	// Print state
	if (log.references)
	{
		for (ObjectDefinitionPair& definitionPair : environment.definitions)
		{
			ObjectDefinition& definition = definitionPair.second;
			printf("%s %s:\n", objectTypeToString(definition.type),
			       definition.name->contents.c_str());
			for (ObjectReferenceStatusPair& reference : definition.references)
			{
				ObjectReferenceStatus& referenceStatus = reference.second;
				printf("\t%s\n", referenceStatus.name->contents.c_str());
			}
		}
	}

	// Keep propagating and evaluating until no more references are resolved.
	// This must be done in passes in case evaluation created more definitions. There's probably a
	// smarter way, but I'll do it in this brute-force style first
	int numReferencesResolved = 0;
	int numBuildResolveErrors = 0;
	do
	{
		PropagateRequiredToReferences(environment);
		numReferencesResolved = BuildEvaluateReferences(environment, numBuildResolveErrors);
		if (numBuildResolveErrors)
			break;
	} while (numReferencesResolved);

	// Check whether everything is resolved
	printf("\nResult:\n");
	int errors = 0;
	for (const ObjectDefinitionPair& definitionPair : environment.definitions)
	{
		const ObjectDefinition& definition = definitionPair.second;
		if (definition.isRequired)
		{
			if (isCompileTimeObject(definition.type))
			{
				// TODO: Add ready-build runtime function check
				if (!findMacro(environment, definition.name->contents.c_str()) &&
				    !findGenerator(environment, definition.name->contents.c_str()))
				{
					// TODO: Add note for who required the object
					ErrorAtToken(*definition.name, "Failed to build required object");
					++errors;
				}
				else
				{
					// NoteAtToken(*definition.name, "built successfully");
				}
			}
			else
			{
				// Check all references have been resolved for regular generated code
				std::vector<const Token*> missingDefinitionNames;
				for (const ObjectReferenceStatusPair& reference : definition.references)
				{
					const ObjectReferenceStatus& referenceStatus = reference.second;

					// TODO: (Performance) Add shortcut in reference
					ObjectDefinitionMap::iterator findIt =
					    environment.definitions.find(referenceStatus.name->contents);
					if (findIt != environment.definitions.end())
					{
						if (isCompileTimeObject(findIt->second.type) &&
						    !isCompileTimeCodeLoaded(environment, findIt->second))
						{
							missingDefinitionNames.push_back(findIt->second.name);
							++errors;
						}
					}

					if (referenceStatus.guessState == GuessState_None)
					{
						ErrorAtToken(*referenceStatus.name, "reference has not been resolved");
					}
				}

				if (!missingDefinitionNames.empty())
				{
					ErrorAtTokenf(*definition.name, "failed to generate %s",
					              definition.name->contents.c_str());
					for (const Token* missingDefinitionName : missingDefinitionNames)
						NoteAtToken(*missingDefinitionName,
						            "missing compile-time function defined here");
				}
			}
		}
		else
		{
			NoteAtToken(*definition.name, "omitted (not required by module)");
		}
	}

	return errors == 0 && numBuildResolveErrors == 0;
}

// This serves only as a warning. I want to be very explicit with the lifetime of tokens
EvaluatorEnvironment::~EvaluatorEnvironment()
{
	if (!macroExpansions.empty())
	{
		printf(
		    "Warning: environmentDestroyInvalidateTokens() has not been called. This will leak "
		    "memory.\n Call it once you are certain no tokens in any expansions will be "
		    "referenced.\n");
	}
}

void environmentDestroyInvalidateTokens(EvaluatorEnvironment& environment)
{
	for (ObjectReferencePoolPair& referencePoolPair : environment.referencePools)
	{
		for (ObjectReference& reference : referencePoolPair.second.references)
		{
			delete reference.spliceOutput;
		}
		referencePoolPair.second.references.clear();
	}
	environment.referencePools.clear();

	for (ObjectDefinitionPair& definitionPair : environment.definitions)
	{
		ObjectDefinition& definition = definitionPair.second;
		delete definition.output;
	}
	environment.definitions.clear();

	for (const std::vector<Token>* macroExpansion : environment.macroExpansions)
		delete macroExpansion;
	environment.macroExpansions.clear();
}

const char* evaluatorScopeToString(EvaluatorScope expectedScope)
{
	switch (expectedScope)
	{
		case EvaluatorScope_Module:
			return "module";
		case EvaluatorScope_Body:
			return "body";
		case EvaluatorScope_ExpressionsOnly:
			return "expressions-only";
		default:
			return "unknown";
	}
}

const char* objectTypeToString(ObjectType type)
{
	switch (type)
	{
		case ObjectType_Function:
			return "Function";
		case ObjectType_CompileTimeMacro:
			return "Macro";
		case ObjectType_CompileTimeGenerator:
			return "Generator";
		default:
			return "Unknown";
	}
}

void resetGeneratorOutput(GeneratorOutput& output)
{
	output.source.clear();
	output.header.clear();
	output.functions.clear();
	output.imports.clear();
}
