/**
 * Cloudflare Worker for ESP32AI Voice Command Processing
 * 
 * Copyright (c) 2026 Xorlent
 * Licensed under the MIT License.
 * https://github.com/Xorlent/ESP32AI
 * 
 * This Worker receives audio from devices running ESP32AI, transcribes using Whisper,
 * and matches commands to device skills using keyword matching or Mistral 7B Instruct.
 * 
 * To deploy, create a free Cloudflare account and follow these steps:
 * 1. Go to Cloudflare Dashboard > Compute > Workers & Pages
 * 2. Create Application > "Start with Hello World"
 * 3. Click "Deploy" then click "</> Edit code"
 * 4. Copy this entire code into the editor, overwriting the Hello World example
 * 5. Click "Deploy," then click the back link at the top left of the editor page
 * 6. Go to Bindings > Add binding
 *    - Select "Workers AI"
 *    - Variable name: "AI" (must be exactly "AI")
 *    - Click "Add Binding"
 * 7. Go to Settings > Variables, add a secret named AUTH_TOKEN set to "Bearer <somestrongandrandomvalue>"
 * 8. Note the worker URL (e.g., https://your-worker.your-subdomain.workers.dev)
 * 9. Use the configured AUTH_TOKEN and this URL during ESP32AI setup
 * 
 * Required: Cloudflare account with Workers AI enabled
 * 
 * NOTE: The ESP32's authorization parameter must exactly match the Cloudflare Worker AUTH_TOKEN value!
 */

export default {
  async fetch(request, env, ctx) {
  // Only accept POST requests
  if (request.method !== "POST") {
    return new Response("Method not allowed.", { status: 405 });
  }

  // Only accept requests to /api/process route
  const url = new URL(request.url);
  if (url.pathname !== "/api/process") {
    return new Response("Not found.", { status: 404 });
  }

    // Verify AUTH_TOKEN is configured
    if (!env.AUTH_TOKEN || env.AUTH_TOKEN.length <= 10) {
      console.log("AUTH_TOKEN not configured or too short (> 10 characters required)");
      return new Response(
        JSON.stringify({ error: "Server configuration error: setup incomplete" }),
        { status: 417, headers: { "Content-Type": "application/json" } }
      );
    }

    // Verify authorization header
    const authHeader = request.headers.get("Authorization");
    if (!authHeader || authHeader !== env.AUTH_TOKEN) {
      console.log("Unauthorized request attempt");
      return new Response(
        JSON.stringify({ error: "Unauthorized" }),
        { status: 401, headers: { "Content-Type": "application/json" } }
      );
    }

    try {
      // Check if AI binding is configured
      if (!env.AI) {
        console.error("AI binding not configured! Go to Worker Settings > Bindings and add a Workers AI binding named 'AI'");
        return new Response(
          JSON.stringify({ error: "Server configuration error: AI binding not found" }),
          { status: 500, headers: { "Content-Type": "application/json" } }
        );
      }

      // Parse multipart form data
      const formData = await request.formData();
      
      const deviceId = formData.get("deviceId");
      const skillsJson = formData.get("skills");
      const audioFile = formData.get("audio");

      // Validate required fields
      if (!deviceId || !skillsJson || !audioFile) {
        console.log("Missing required fields");
        // Return empty array so ESP32 gets valid response (no action)
        return new Response(
          JSON.stringify([]),
          { status: 400, headers: { "Content-Type": "application/json" } }
        );
      }

      console.log(`Processing request from device: ${deviceId}`);

      // Parse skills JSON
      let skills;
      try {
        skills = JSON.parse(skillsJson);
        if (!Array.isArray(skills) || skills.length === 0) {
          throw new Error("Skills must be a non-empty array");
        }
        console.log(`Skills configured: ${skills.map(s => s.Target).join(', ')}`);
      } catch (e) {
        console.log("Invalid skills JSON format:", e);
        // Return empty array so ESP32 gets valid response
        return new Response(
          JSON.stringify([]),
          { status: 400, headers: { "Content-Type": "application/json" } }
        );
      }

      // Step 1: Transcribe audio using Whisper
      console.log("Transcribing audio with Whisper...");
      const audioBlob = await audioFile.arrayBuffer();
      console.log(`Audio size: ${audioBlob.byteLength} bytes`);
      
      const whisperResponse = await env.AI.run(
        "@cf/openai/whisper",
        {
          audio: Array.from(new Uint8Array(audioBlob)),
        }
      );

      const transcription = whisperResponse.text || "";
      console.log(`Transcription: "${transcription}"`);

      if (!transcription || transcription.trim().length === 0) {
        console.log("No speech detected in audio");
        return createNoActionResponse(skills);
      }

      // Step 2: Match transcription to skills using Mistral 7B Instruct
      const matchedAction = await matchTranscriptionToSkills(
        env.AI,
        transcription,
        skills
      );

      console.log(`Matched action:`, matchedAction);

      // Step 3: Return response
      return new Response(JSON.stringify(matchedAction), {
        status: 200,
        headers: { "Content-Type": "application/json" },
      });

    } catch (error) {
      console.error("Error processing request:", error);
      // Return empty array so ESP32 gets valid response (no action)
      return new Response(
        JSON.stringify([]),
        { status: 500, headers: { "Content-Type": "application/json" } }
      );
    }
  }
};

/**
 * Match transcribed text to device skills
 * Uses keyword matching first, then falls back to Mistral 7B Instruct AI for ambiguous cases
 */
async function matchTranscriptionToSkills(
  ai,
  transcription,
  skills
) {
  
  // Step 1: Try fast keyword matching first
  console.log("Attempting keyword matching...");
  const keywordResult = keywordMatch(transcription, skills);
  
  // If null is returned, it means ambiguous match - skip to AI
  if (keywordResult === null) {
    console.log("Ambiguity detected, using Mistral AI for disambiguation...");
  } else {
    // Check if we found a real match (not all "None" actions)
    const hasMatch = keywordResult.some(action => action.Action !== "None");
    
    if (hasMatch) {
      console.log("✓ Keyword match found, skipping AI");
      return keywordResult;
    }
  }
  
  // Step 2: No keyword match found or ambiguous match, use Mistral AI
  console.log("Using Mistral AI to resolve command...");
  
  // Build a detailed prompt explaining the device capabilities
  let prompt = `You are analyzing a voice command for a smart device. The device has the following capabilities:\n\n`;
  
  for (const skill of skills) {
    prompt += `Target: ${skill.Target}\n`;
    prompt += `Available Actions:\n`;
    
    for (const action of skill.Actions) {
      prompt += `  - ${action.Name} (${action.Type}): Can be triggered by phrases like "${action.Variants.join('", "')}"\n`;
    }
    prompt += `\n`;
  }
  
  prompt += `\nWe think the user said: "${transcription}"\n\n`;
  prompt += `Task: Determine if this transcribed voice command is a close enough match for any of the available actions.`;
  prompt += `- NOTE: Transcriptions can be inexact.  Example: "porch on" could read as "for chon" or "report on" or "port john" or "force on"`;
  prompt += `- Target names generally refer to a device or a location.  Be sure to consider this when matching since there can be many different commonly used names for a location or type of device.`
  prompt += `Respond with ONLY the target name and action name in this exact format: "MATCH: TargetName -> ActionName".\n`;
  prompt += `If the user is setting a numeric value (like temperature), include it like: "MATCH: TargetName -> ActionName VALUE: 72".\n`;
  prompt += `If it doesn't match any action, respond with exactly: "NO_MATCH". DO NOT GIVE ANY ANALYSIS WITH YOUR RESPONSE.\n\n`;
  prompt += `Response:`;

  try {
    // Use Mistral 7B Instruct for command classification
    const mistralResponse = await ai.run(
      "@hf/mistral/mistral-7b-instruct-v0.2",
      {
        messages: [
          { role: "user", content: prompt }
        ],
        max_tokens: 100,
        temperature: .8,
      }
    );

    const response = mistralResponse.response || "";
    console.log(`Mistral response: "${response}"`);

    // Parse Mistral's response
    const matchResult = parseMatchResult(response, transcription, skills);
    
    return matchResult;

  } catch (error) {
    console.error("Error with Mistral analysis:", error);
    // If AI fails, return no action
    return createNoActionResponse(skills);
  }
}

/**
 * Parse Mistral's response to extract the matched action
 */
function parseMatchResult(
  aiResponse,
  transcription,
  skills
) {
  
  console.log(`Parsing AI response (${aiResponse.length} chars)`);
  
  const upperResponse = aiResponse.toUpperCase();
  
  // Check for explicit NO_MATCH
  if (upperResponse.includes("NO_MATCH") || upperResponse.includes("NO MATCH")) {
    console.log("✓ Parsed: NO_MATCH detected");
    return createNoActionResponse(skills);
  }

  // Primary pattern: MATCH: Target -> Action [VALUE: number]
  // Handles: 'MATCH: Porch -> On' or '"MATCH: Porch -> On"' or embedded in text
  const matchPattern = /MATCH:\s*([\w\s]+?)\s*->\s*([\w\s]+?)(?:\s+VALUE:\s*(\d+))?(?:["\s.]|$)/i;
  const match = aiResponse.match(matchPattern);

  if (match) {
    const targetName = match[1].trim();
    const actionName = match[2].trim();
    const value = match[3];
    
    console.log(`✓ Parsed MATCH pattern: "${targetName}" -> "${actionName}"${value ? ` (value: ${value})` : ''}`);
    
    // Verify the target and action exist in skills (case-insensitive, flexible matching)
    const skill = skills.find(s => 
      s.Target.toLowerCase().replace(/\s+/g, '') === targetName.toLowerCase().replace(/\s+/g, '')
    );
    
    if (skill) {
      const action = skill.Actions.find(a => 
        a.Name.toLowerCase().replace(/\s+/g, '') === actionName.toLowerCase().replace(/\s+/g, '')
      );
      
      if (action) {
        console.log(`✓ Verified: ${skill.Target}.${action.Name}`);
        return createActionResponse(skills, skill.Target, action.Name, value);
      } else {
        console.log(`⚠ Action "${actionName}" not found in skill "${skill.Target}"`);
      }
    } else {
      console.log(`⚠ Target "${targetName}" not found in skills`);
    }
  }

  // Fallback: Look for function and action names mentioned in response
  console.log("Primary pattern failed, trying fallback detection...");
  for (const skill of skills) {
    const functionRegex = new RegExp(`\\b${skill.Target.replace(/\s+/g, '\\s+')}\\b`, 'i');
    if (functionRegex.test(aiResponse)) {
      for (const action of skill.Actions) {
        const actionRegex = new RegExp(`\\b${action.Name}\\b`, 'i');
        if (actionRegex.test(aiResponse)) {
          console.log(`✓ Fallback match: Found "${skill.Target}" and "${action.Name}" in response`);
          
          // Check for numeric value if it's an integer action
          let value = undefined;
          if (action.Type === "integer") {
            const numberMatch = aiResponse.match(/\b(\d+)\b/);
            if (numberMatch) {
              value = numberMatch[1];
            }
          }
          
          return createActionResponse(skills, skill.Target, action.Name, value);
        }
      }
    }
  }

  // If AI response wasn't clear, return no action
  console.log("✗ Could not parse AI response, returning no action");
  return createNoActionResponse(skills);
}

/**
 * Keyword matching for command recognition
 */
function keywordMatch(
  transcription,
  skills
) {
  
  const lowerTranscription = transcription.toLowerCase();
  
  // Track matches per target to detect ambiguity
  const targetMatches = new Map(); // Map<targetName, {skill, action, score, value}>

  // Try to extract numeric value for "Set" actions
  const numberMatch = lowerTranscription.match(/\b(\d+)\b/);
  const extractedNumber = numberMatch ? numberMatch[1] : undefined;

  console.log(`Keyword matching transcription: "${transcription}"`);

  for (const skill of skills) {
    // First check for direct target+action pattern (e.g., "porch on", "living room off")
    // This handles common speech patterns where users say the location/device name + action
    const targetWords = skill.Target.toLowerCase().split(/\s+/);
    let targetFound = true;
    let lastTargetIndex = -1;
    
    // Check if all target name words appear in order in transcription
    for (const word of targetWords) {
      const index = lowerTranscription.indexOf(word, lastTargetIndex + 1);
      if (index === -1) {
        targetFound = false;
        break;
      }
      lastTargetIndex = index;
    }
    
    if (targetFound) {
      // Target name found, now check if action name appears after it
      for (const action of skill.Actions) {
        const actionWords = action.Name.toLowerCase().split(/\s+/);
        let actionFound = true;
        let lastActionIndex = lastTargetIndex;
        
        for (const word of actionWords) {
          const index = lowerTranscription.indexOf(word, lastActionIndex + 1);
          if (index === -1) {
            actionFound = false;
            break;
          }
          lastActionIndex = index;
        }
        
        if (actionFound) {
          // Direct target+action match (e.g., "porch on")
          const score = (skill.Target.length + action.Name.length) * 3; // High priority for direct matches
          console.log(`Direct match found: "${skill.Target} ${action.Name}" (score: ${score})`);
          
          const currentBest = targetMatches.get(skill.Target);
          if (!currentBest || score > currentBest.score) {
            targetMatches.set(skill.Target, {
              skill,
              action,
              score,
              value: action.Type === "integer" ? extractedNumber : undefined,
            });
          }
        }
      }
    }
    
    // Also check traditional variants
    for (const action of skill.Actions) {
      // Check each variant
      for (const variant of action.Variants) {
        const lowerVariant = variant.toLowerCase();
        
        // Check if transcription contains this variant (exact or with words in between)
        // Split variant into words and check if all words appear in order
        const variantWords = lowerVariant.split(/\s+/);
        let matchFound = true;
        let lastIndex = -1;
        
        for (const word of variantWords) {
          const index = lowerTranscription.indexOf(word, lastIndex + 1);
          if (index === -1) {
            matchFound = false;
            break;
          }
          lastIndex = index;
        }
        
        if (matchFound) {
          // Calculate match score (longer variants score higher)
          let score = lowerVariant.length;
          
          // Add bonus points for matching words in the target name
          // This helps differentiate between "porch light" and "living room light"
          const targetWords = skill.Target.toLowerCase().split(/\s+/);
          let targetMatchBonus = 0;
          for (const word of targetWords) {
            // Skip common words that don't help with disambiguation
            if (word.length > 3 && lowerTranscription.includes(word)) {
              targetMatchBonus += word.length * 2; // Weight target name matches heavily
            }
          }
          score += targetMatchBonus;
          
          console.log(`Match found: "${variant}" in ${skill.Target}.${action.Name} (score: ${score})`);
          
          // Track the best match for this target
          const currentBest = targetMatches.get(skill.Target);
          if (!currentBest || score > currentBest.score) {
            targetMatches.set(skill.Target, {
              skill,
              action,
              score,
              value: action.Type === "integer" ? extractedNumber : undefined,
            });
          }
        }
      }
    }
  }

  // Check if we have any matches
  if (targetMatches.size === 0) {
    console.log("No keyword matches found");
    return createNoActionResponse(skills);
  }

  // Find the top score(s)
  let topScore = 0;
  for (const match of targetMatches.values()) {
    if (match.score > topScore) {
      topScore = match.score;
    }
  }

  // Count how many targets have the top score (or very close to it)
  // Consider scores within 10% of top score as ambiguous
  const ambiguityThreshold = Math.max(1, Math.floor(topScore * 0.1));
  const topMatches = [];
  
  for (const [targetName, match] of targetMatches.entries()) {
    if (match.score >= topScore - ambiguityThreshold) {
      topMatches.push({ targetName, ...match });
    }
  }

  // If multiple targets have similar scores, it's ambiguous - defer to LLM
  if (topMatches.length > 1) {
    const ambiguousTargets = topMatches.map(m => `${m.targetName} (score: ${m.score})`).join(', ');
    console.log(`⚠ Ambiguous match detected: ${topMatches.length} targets with similar scores: ${ambiguousTargets}`);
    console.log("Deferring to LLM for disambiguation...");
    return null; // Signal that we should use LLM instead
  }

  // Clear winner - return the best match
  const bestMatch = topMatches[0];
  console.log(`Best match selected: ${bestMatch.skill.Target} -> ${bestMatch.action.Name}`);
  return createActionResponse(
    skills,
    bestMatch.skill.Target,
    bestMatch.action.Name,
    bestMatch.value
  );
}

/**
 * Create response with a matched action
 */
function createActionResponse(
  skills,
  matchedTarget,
  matchedAction,
  value
) {
  
  const response = [];

  for (const skill of skills) {
    if (skill.Target === matchedTarget) {
      const actionResponse = {
        Target: skill.Target,
        Action: matchedAction,
      };
      
      if (value) {
        actionResponse.Value = value;
      }
      
      response.push(actionResponse);
    } else {
      response.push({
        Target: skill.Target,
        Action: "None",
      });
    }
  }

  return response;
}

/**
 * Create response with no matched actions
 */
function createNoActionResponse(skills) {
  return skills.map(skill => ({
    Target: skill.Target,
    Action: "None",
  }));
}
